#include "video/ColorConverter.h"

#include <windows.h>

#include <d3d11.h>
#include <d3d11_4.h>  // ID3D11Multithread
#include <wrl/client.h>

#include "util/Log.h"

using Microsoft::WRL::ComPtr;

namespace gl {

struct ColorConverter::Interno {
    ComPtr<ID3D11Device> dispositivo;
    ComPtr<ID3D11DeviceContext> contexto;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContexto;
    ComPtr<ID3D11VideoProcessor> processador;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerador;
    // Anel de saída: o encoder é assíncrono e pode estar lendo o quadro
    // anterior enquanto o próximo é convertido. Escrever sempre na mesma
    // textura fazia o encoder receber quadros meio sobrescritos e travar.
    static constexpr size_t kSaidas = 4;
    ComPtr<ID3D11Texture2D> saidaNv12[kSaidas];
    ComPtr<ID3D11VideoProcessorOutputView> vistaSaida[kSaidas];
    size_t proximaSaida = 0;

    uint32_t larguraEntrada = 0;
    uint32_t alturaEntrada = 0;
    uint32_t largura = 0;
    uint32_t altura = 0;

    // Quantas imagens entram no quadro. Uma é o caminho de sempre.
    UINT pedacos = 1;

    // Para não repetir a mesma linha de erro a cada quadro.
    HRESULT ultimoErroVista = S_OK;
};

ColorConverter::ColorConverter() : d_(std::make_unique<Interno>()) {}
ColorConverter::~ColorConverter() = default;

bool ColorConverter::iniciar(ID3D11Device* dispositivo, ID3D11DeviceContext* contexto,
                             uint32_t larguraEntrada, uint32_t alturaEntrada,
                             uint32_t larguraSaida, uint32_t alturaSaida,
                             Saida formatoSaida, uint32_t graus) {
    d_->dispositivo = dispositivo;
    d_->contexto = contexto;
    d_->larguraEntrada = larguraEntrada;
    d_->alturaEntrada = alturaEntrada;

    // O H.264 trabalha em macroblocos de 16x16 e a componente de cor do NV12 é
    // subamostrada pela metade. Dimensão ímpar aqui vira erro na criação da
    // textura ou artefato na borda, então arredonda para par.
    d_->largura = larguraSaida & ~1u;
    d_->altura = alturaSaida & ~1u;

    HRESULT resultado = d_->dispositivo.As(&d_->videoDevice);
    if (FAILED(resultado)) {
        erro("ID3D11VideoDevice indisponivel: {}", hr(resultado));
        return false;
    }
    resultado = d_->contexto.As(&d_->videoContexto);
    if (FAILED(resultado)) {
        erro("ID3D11VideoContext indisponivel: {}", hr(resultado));
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC descricao{};
    descricao.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    descricao.InputWidth = larguraEntrada;
    descricao.InputHeight = alturaEntrada;
    descricao.OutputWidth = d_->largura;
    descricao.OutputHeight = d_->altura;
    // Dica de uso, não obrigação: pede o caminho mais rápido em vez do de
    // melhor qualidade, que é o certo para tempo real.
    descricao.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    resultado = d_->videoDevice->CreateVideoProcessorEnumerator(&descricao, &d_->enumerador);
    if (FAILED(resultado)) {
        erro("CreateVideoProcessorEnumerator falhou: {}", hr(resultado));
        return false;
    }
    resultado = d_->videoDevice->CreateVideoProcessor(d_->enumerador.Get(), 0, &d_->processador);
    if (FAILED(resultado)) {
        erro("CreateVideoProcessor falhou: {}", hr(resultado));
        return false;
    }

    // Pergunta ao driver o que ele aceita, em vez de supor.
    //
    // O Video Processor de cada placa suporta um conjunto diferente de
    // formatos, e ele recusa o resto com E_INVALIDARG na hora de criar a vista
    // - um erro que nao diz qual formato nem em qual direcao. Perguntar antes
    // transforma isso numa linha de log que aponta o problema direto.
    const DXGI_FORMAT formatoDeEntrada =
        (formatoSaida == Saida::Bgra) ? DXGI_FORMAT_NV12 : DXGI_FORMAT_B8G8R8A8_UNORM;
    const DXGI_FORMAT formatoDeSaida =
        (formatoSaida == Saida::Bgra) ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_NV12;

    UINT suporteEntrada = 0;
    UINT suporteSaida = 0;
    d_->enumerador->CheckVideoProcessorFormat(formatoDeEntrada, &suporteEntrada);
    d_->enumerador->CheckVideoProcessorFormat(formatoDeSaida, &suporteSaida);

    if (!(suporteEntrada & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        erro("o Video Processor desta placa nao aceita este formato na entrada");
        return false;
    }
    if (!(suporteSaida & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        erro("o Video Processor desta placa nao aceita este formato na saida");
        return false;
    }

    D3D11_TEXTURE2D_DESC saida{};
    saida.Width = d_->largura;
    saida.Height = d_->altura;
    saida.MipLevels = 1;
    saida.ArraySize = 1;
    saida.Format = (formatoSaida == Saida::Bgra) ? DXGI_FORMAT_B8G8R8A8_UNORM
                                                 : DXGI_FORMAT_NV12;
    saida.SampleDesc.Count = 1;
    saida.Usage = D3D11_USAGE_DEFAULT;
    // SHADER_RESOURCE junto com VIDEO_ENCODER: o encoder por hardware lê como
    // recurso, e sem essa flag alguns drivers recusam a textura.
    saida.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC vista{};
    vista.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;

    for (size_t i = 0; i < Interno::kSaidas; ++i) {
        resultado = d_->dispositivo->CreateTexture2D(&saida, nullptr, &d_->saidaNv12[i]);
        if (FAILED(resultado)) {
            erro("nao foi possivel criar a textura NV12: {}", hr(resultado));
            return false;
        }
        resultado = d_->videoDevice->CreateVideoProcessorOutputView(
            d_->saidaNv12[i].Get(), d_->enumerador.Get(), &vista, &d_->vistaSaida[i]);
        if (FAILED(resultado)) {
            erro("CreateVideoProcessorOutputView falhou: {}", hr(resultado));
            return false;
        }
    }

    // O encoder por hardware toca no dispositivo D3D11 a partir da thread dele.
    // Sem esta protecao os dois lados mexem no mesmo contexto ao mesmo tempo: o
    // MFT para de pedir entrada e a codificacao morre depois de um ou dois
    // quadros, sem erro nenhum aparecer.
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(d_->dispositivo.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    } else {
        aviso("ID3D11Multithread indisponivel; o encoder pode ficar instavel");
    }

    // Sem isto o processador tenta manter o histórico entre quadros, o que só
    // faz sentido para desentrelaçar. Aqui cada quadro é independente.
    d_->videoContexto->VideoProcessorSetStreamFrameFormat(d_->processador.Get(), 0,
                                                          D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    d_->videoContexto->VideoProcessorSetStreamAutoProcessingMode(d_->processador.Get(), 0, FALSE);

    // Giro do monitor.
    //
    // A duplicação entrega a textura na orientação FÍSICA do painel: um monitor
    // 1920x1080 posto em pé continua entregando 1920x1080, com o conteúdo
    // deitado. Quem via a transmissão recebia a tela virada de lado.
    //
    // O Video Processor gira de graça, na mesma passada da conversão de cor -
    // é a unidade de função fixa da placa, não custa quadro nenhum. Fazer isso
    // com shader ou na CPU custaria os dois.
    if (graus != 0) {
        ComPtr<ID3D11VideoContext1> videoContexto1;
        if (SUCCEEDED(d_->videoContexto.As(&videoContexto1))) {
            const auto giro = (graus == 90)    ? D3D11_VIDEO_PROCESSOR_ROTATION_90
                              : (graus == 180) ? D3D11_VIDEO_PROCESSOR_ROTATION_180
                                               : D3D11_VIDEO_PROCESSOR_ROTATION_270;
            videoContexto1->VideoProcessorSetStreamRotation(d_->processador.Get(), 0, TRUE, giro);
            info("monitor girado {} graus: corrigindo na GPU", graus);
        } else {
            aviso("monitor girado {} graus, mas o driver nao expoe rotacao no Video Processor",
                  graus);
        }
    }

    // Diz o formato de verdade: o mesmo Video Processor serve os dois sentidos,
    // e um log que sempre escreve "NV12" mente na metade das vezes.
    info("conversao {}x{} -> {} {}x{} na GPU", larguraEntrada, alturaEntrada,
         formatoSaida == Saida::Nv12 ? "NV12" : "BGRA", d_->largura, d_->altura);
    return true;
}

// Monta onde cada imagem cai dentro do quadro final.
//
// Uma tela sozinha ocupa tudo; duas telas ficam lado a lado; a câmera vai num
// canto. Quem decide o recorte é quem chama - aqui só se diz ao Video
// Processor, uma vez, e ele repete a cada quadro sem custo.
bool ColorConverter::prepararComposicao(const std::vector<Pedaco>& pedacos) {
    if (!d_->processador || pedacos.empty()) return false;

    // A placa precisa aceitar NV12 na entrada: no caminho do encoder a entrada
    // principal é BGRA, então esse formato ainda não foi conferido - e é o da
    // câmera.
    UINT suporte = 0;
    d_->enumerador->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &suporte);
    const bool aceitaNv12 = (suporte & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) != 0;

    D3D11_VIDEO_PROCESSOR_CAPS capacidades{};
    if (FAILED(d_->enumerador->GetVideoProcessorCaps(&capacidades))) return false;

    const size_t cabem = (std::min)(static_cast<size_t>(capacidades.MaxInputStreams),
                                    static_cast<size_t>(capacidades.MaxStreamStates));
    if (pedacos.size() > cabem) {
        aviso("o Video Processor desta placa aceita {} entradas, e foram pedidas {}", cabem,
              pedacos.size());
        return false;
    }
    if (pedacos.size() > 1 && !aceitaNv12) {
        aviso("o Video Processor desta placa nao aceita NV12 na entrada; sem composicao");
        return false;
    }

    for (size_t i = 0; i < pedacos.size(); ++i) {
        const RECT destino{pedacos[i].esquerda, pedacos[i].topo, pedacos[i].direita,
                           pedacos[i].baixo};
        const auto fluxo = static_cast<UINT>(i);
        // Dizer o destino de TODOS, inclusive o primeiro: com mais de uma
        // entrada o processador não presume mais que a primeira é o fundo.
        d_->videoContexto->VideoProcessorSetStreamDestRect(d_->processador.Get(), fluxo, TRUE,
                                                           &destino);
        d_->videoContexto->VideoProcessorSetStreamFrameFormat(
            d_->processador.Get(), fluxo, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        d_->videoContexto->VideoProcessorSetStreamAutoProcessingMode(d_->processador.Get(), fluxo,
                                                                     FALSE);

        // Giro por imagem: a duplicacao entrega na orientacao FISICA do painel,
        // e um monitor em pe chega deitado. O Video Processor gira de graca, na
        // mesma passada - fazer isso com shader custaria uma passada inteira.
        if (pedacos[i].graus != 0) {
            ComPtr<ID3D11VideoContext1> videoContexto1;
            if (SUCCEEDED(d_->videoContexto.As(&videoContexto1))) {
                const auto giro = (pedacos[i].graus == 90)    ? D3D11_VIDEO_PROCESSOR_ROTATION_90
                                  : (pedacos[i].graus == 180) ? D3D11_VIDEO_PROCESSOR_ROTATION_180
                                                              : D3D11_VIDEO_PROCESSOR_ROTATION_270;
                videoContexto1->VideoProcessorSetStreamRotation(d_->processador.Get(), fluxo, TRUE,
                                                               giro);
            }
        }
    }

    // O que sobrar entre os pedaços fica preto, e não com lixo do quadro
    // anterior. Com duas telas de proporções diferentes lado a lado sempre
    // sobra faixa.
    const D3D11_VIDEO_COLOR preto{{0.0f, 0.0f, 0.0f, 1.0f}};
    d_->videoContexto->VideoProcessorSetOutputBackgroundColor(d_->processador.Get(), FALSE, &preto);

    d_->pedacos = static_cast<UINT>(pedacos.size());
    return true;
}

void ColorConverter::desligarComposicao() {
    if (d_->pedacos <= 1) return;
    for (UINT i = 1; i < d_->pedacos; ++i) {
        d_->videoContexto->VideoProcessorSetStreamDestRect(d_->processador.Get(), i, FALSE,
                                                           nullptr);
    }
    d_->pedacos = 1;
}

ID3D11Texture2D* ColorConverter::compor(const std::vector<ID3D11Texture2D*>& entradas) {
    if (!d_->processador || entradas.empty()) return nullptr;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC descricao{};
    descricao.FourCC = 0;
    descricao.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    descricao.Texture2D.MipSlice = 0;
    descricao.Texture2D.ArraySlice = 0;

    // As vistas são criadas por quadro. Guardá-las não adiantaria: as texturas
    // andam em rodízio, e a vista fica presa à textura de onde saiu.
    //
    // Elas precisam continuar VIVAS até o Blt terminar, e por isso ficam neste
    // vetor em vez de numa variável dentro do laço.
    std::vector<ComPtr<ID3D11VideoProcessorInputView>> vistas(entradas.size());
    std::vector<D3D11_VIDEO_PROCESSOR_STREAM> fluxos(entradas.size());

    // Nenhuma superfície nula chega ao Blt.
    //
    // O desenho anterior mandava o fluxo com Enable = FALSE e pInputSurface
    // nulo quando a imagem ainda não tinha chegado, contando com o processador
    // ignorar o fluxo desligado. O driver da AMD não ignora: ele lê o ponteiro
    // antes de olhar o Enable, e o processo morre com violação de acesso
    // dentro do driver - sem log, sem registro no Windows, saída 139.
    //
    // Truncar na primeira que falta resolve sem caso especial. A ordem é
    // [telas..., câmera], então o que cai fora é justamente o que veio por
    // último - e volta sozinho no quadro seguinte.
    UINT total = 0;
    for (size_t i = 0; i < entradas.size(); ++i) {
        fluxos[i] = {};
        if (!entradas[i]) break;

        const HRESULT resultado = d_->videoDevice->CreateVideoProcessorInputView(
            entradas[i], d_->enumerador.Get(), &descricao, &vistas[i]);
        if (FAILED(resultado)) {
            // Uma linha por quadro encheria o log; só a primeira interessa.
            if (d_->ultimoErroVista != resultado) {
                d_->ultimoErroVista = resultado;
                erro("CreateVideoProcessorInputView falhou: {}", hr(resultado));
            }
            break;
        }
        fluxos[i].Enable = TRUE;
        fluxos[i].pInputSurface = vistas[i].Get();
        total = static_cast<UINT>(i + 1);
    }

    if (total == 0) return nullptr;

    const size_t indice = d_->proximaSaida;
    d_->proximaSaida = (d_->proximaSaida + 1) % Interno::kSaidas;

    const HRESULT resultado = d_->videoContexto->VideoProcessorBlt(
        d_->processador.Get(), d_->vistaSaida[indice].Get(), 0, total, fluxos.data());
    if (FAILED(resultado)) {
        erro("VideoProcessorBlt falhou: {}", hr(resultado));
        return nullptr;
    }
    return d_->saidaNv12[indice].Get();
}

uint32_t ColorConverter::largura() const { return d_->largura; }
uint32_t ColorConverter::altura() const { return d_->altura; }

}  // namespace gl
