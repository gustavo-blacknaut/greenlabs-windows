#include "decoder/VideoDecoder.h"

#include <windows.h>

#include <d3d10_1.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <vector>

#include "util/Log.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace gl {
namespace {

std::string nomeDoAtivador(IMFActivate* ativador) {
    LPWSTR bruto = nullptr;
    UINT32 tamanho = 0;
    if (FAILED(ativador->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &bruto, &tamanho))) {
        return "desconhecido";
    }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, bruto, -1, nullptr, 0, nullptr, nullptr);
    std::string nome(n > 1 ? static_cast<size_t>(n - 1) : 0, '\0');
    if (n > 1) ::WideCharToMultiByte(CP_UTF8, 0, bruto, -1, nome.data(), n, nullptr, nullptr);
    ::CoTaskMemFree(bruto);
    return nome;
}

}  // namespace

struct VideoDecoder::Interno {
    ComPtr<IMFTransform> transformador;
    ComPtr<IMFDXGIDeviceManager> gerenciador;
    ComPtr<ID3D11Device> dispositivo;
    ComPtr<ID3D11DeviceContext> contexto;

    // Cópia própria do quadro. O MFT entrega uma fatia de um array de texturas
    // que ele reaproveita a cada quadro; entregar isso direto ao renderizador
    // daria imagem trocando embaixo dele.
    ComPtr<ID3D11Texture2D> quadro;
    uint32_t largura = 0;
    uint32_t altura = 0;

    // Resolucao do tipo de saida negociado. O caminho de software entrega
    // bytes soltos, sem dimensao junto: sem isto nao da para montar a textura.
    uint32_t larguraSaida = 0;
    uint32_t alturaSaida = 0;

    Consumidor consumidor;
    std::string nome;
    bool iniciado = false;
    bool tipoSaidaPronto = false;
    bool assincrono = false;
    bool avisouCaminho = false;
    bool hardware = false;
    ComPtr<IMFMediaEventGenerator> eventos;
    int entradasPedidas = 0;
    void drenarEventos();
    UINT resetToken = 0;

    std::atomic<uint64_t> quadros{0};

    // Contadores de diagnostico: sem eles, "nao aparece imagem" e indistinguivel
    // de "nao chegou dado", "o MFT recusou" e "o MFT engoliu sem devolver".
    uint64_t entradas = 0;
    uint64_t entradasRecusadas = 0;
    uint64_t saidasVazias = 0;
    uint64_t trocasDeTipo = 0;
    HRESULT ultimoErro = S_OK;
    std::chrono::steady_clock::time_point ultimoRelato = std::chrono::steady_clock::now();
    void relatar();

    bool escolherTipoSaida();
    void colherSaida();
    bool prepararCopia(uint32_t largura, uint32_t altura);
};

VideoDecoder::VideoDecoder() : d_(std::make_unique<Interno>()) {}
VideoDecoder::~VideoDecoder() { parar(); }

bool VideoDecoder::iniciar(ID3D11Device* dispositivo, Consumidor consumidor) {
    if (d_->iniciado) return true;
    d_->dispositivo = dispositivo;
    d_->dispositivo->GetImmediateContext(&d_->contexto);
    d_->consumidor = std::move(consumidor);

    if (FAILED(::MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        erro("MFStartup falhou no decodificador");
        return false;
    }

    MFT_REGISTER_TYPE_INFO entradaDesejada{MFMediaType_Video, MFVideoFormat_H264};

    // Hardware primeiro, como no encoder: sem separar as passadas o MFTEnumEx
    // costuma devolver o de software antes, e a maquina decodifica na CPU sem
    // ninguem perceber.
    for (int passada = 0; passada < 2 && !d_->transformador; ++passada) {
        // Todo decodificador de hardware e ASSINCRONO. Pedir so HARDWARE nao
        // acha nenhum, a busca cai na segunda passada e a maquina decodifica na
        // CPU com a placa parada do lado - foi o que aconteceu aqui.
        UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER;
        flags |= (passada == 0) ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT)
                                : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT);

        IMFActivate** ativadores = nullptr;
        UINT32 total = 0;
        if (FAILED(::MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &entradaDesejada, nullptr,
                               &ativadores, &total)) ||
            total == 0) {
            if (ativadores) ::CoTaskMemFree(ativadores);
            continue;
        }
        for (UINT32 i = 0; i < total; ++i) {
            if (!d_->transformador &&
                SUCCEEDED(ativadores[i]->ActivateObject(IID_PPV_ARGS(&d_->transformador)))) {
                d_->nome = nomeDoAtivador(ativadores[i]);
                d_->hardware = (passada == 0);
            }
            ativadores[i]->Release();
        }
        ::CoTaskMemFree(ativadores);
    }

    if (!d_->transformador) {
        erro("nenhum decodificador de H.264 disponivel nesta maquina");
        return false;
    }

    // Protecao multithread no dispositivo D3D11. O Media Foundation usa o
    // dispositivo de outra thread que nao a nossa; sem esta marca ele RECUSA a
    // aceleracao e cai para software sem dizer nada - e por isso que o log
    // mostrava "Microsoft H264 Video Decoder MFT" decodificando na CPU com uma
    // RX 590 parada do lado.
    {
        ComPtr<ID3D10Multithread> multithread;
        if (SUCCEEDED(d_->dispositivo.As(&multithread))) {
            multithread->SetMultithreadProtected(TRUE);
        }
    }

    // Compartilha o dispositivo D3D11 com o MFT: e o que faz a saida vir como
    // textura na GPU em vez de voltar para a memoria principal.
    if (SUCCEEDED(::MFCreateDXGIDeviceManager(&d_->resetToken, &d_->gerenciador)) &&
        SUCCEEDED(d_->gerenciador->ResetDevice(d_->dispositivo.Get(), d_->resetToken))) {
        d_->transformador->ProcessMessage(
            MFT_MESSAGE_SET_D3D_MANAGER,
            reinterpret_cast<ULONG_PTR>(d_->gerenciador.Get()));
    }

    // Baixa latencia: sem isto o decodificador segura varios quadros para
    // reordenar antes de entregar o primeiro. Numa chamada ao vivo nao ha o que
    // reordenar - o que chega ja esta em ordem - e a espera vira tela preta.
    ComPtr<IMFAttributes> atributos;
    if (SUCCEEDED(d_->transformador->GetAttributes(&atributos))) {
        atributos->SetUINT32(MF_LOW_LATENCY, TRUE);
        atributos->SetUINT32(MF_SA_D3D11_AWARE, TRUE);

        // MFT de hardware nasce trancado: sem destravar, o ProcessInput
        // devolve erro e nenhum quadro entra.
        UINT32 assincrono = 0;
        if (SUCCEEDED(atributos->GetUINT32(MF_TRANSFORM_ASYNC, &assincrono)) && assincrono) {
            atributos->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            d_->assincrono = true;
            d_->transformador.As(&d_->eventos);
        }
    }

    ComPtr<IMFMediaType> tipoEntrada;
    if (FAILED(::MFCreateMediaType(&tipoEntrada))) return false;
    tipoEntrada->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    tipoEntrada->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    tipoEntrada->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    HRESULT resultado = d_->transformador->SetInputType(0, tipoEntrada.Get(), 0);
    if (FAILED(resultado)) {
        erro("SetInputType do decodificador falhou: {}", hr(resultado));
        return false;
    }

    // O tipo de saida so pode ser escolhido depois que o decodificador leu o
    // SPS e sabe a resolucao. Aqui a tentativa costuma falhar, e nao e erro.
    d_->escolherTipoSaida();

    if (d_->assincrono) {
        d_->transformador->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    d_->transformador->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d_->transformador->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    d_->iniciado = true;
    info("decodificador H.264: {} ({}, {})", d_->nome, d_->hardware ? "hardware" : "software",
         d_->assincrono ? "assincrono" : "sincrono");
    return true;
}

bool VideoDecoder::Interno::escolherTipoSaida() {
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> tipo;
        const HRESULT resultado = transformador->GetOutputAvailableType(0, i, &tipo);
        if (resultado == MF_E_NO_MORE_TYPES || FAILED(resultado)) return false;

        GUID subtipo{};
        if (FAILED(tipo->GetGUID(MF_MT_SUBTYPE, &subtipo))) continue;
        if (subtipo != MFVideoFormat_NV12) continue;

        if (SUCCEEDED(transformador->SetOutputType(0, tipo.Get(), 0))) {
            UINT32 l = 0, a = 0;
            ::MFGetAttributeSize(tipo.Get(), MF_MT_FRAME_SIZE, &l, &a);
            larguraSaida = l;
            alturaSaida = a;
            info("decodificador: saida NV12 {}x{}", l, a);
            tipoSaidaPronto = true;
            return true;
        }
    }
}

bool VideoDecoder::Interno::prepararCopia(uint32_t novaLargura, uint32_t novaAltura) {
    if (quadro && largura == novaLargura && altura == novaAltura) return true;

    D3D11_TEXTURE2D_DESC descricao{};
    descricao.Width = novaLargura;
    descricao.Height = novaAltura;
    descricao.MipLevels = 1;
    descricao.ArraySize = 1;
    descricao.Format = DXGI_FORMAT_NV12;
    descricao.SampleDesc.Count = 1;
    descricao.Usage = D3D11_USAGE_DEFAULT;

    // Sem flag de ligacao nenhuma.
    //
    // Esta textura so e destino de copia e entrada do Video Processor - nada
    // aqui a le como recurso de shader. E o SHADER_RESOURCE que estava aqui era
    // justamente o que fazia o CreateVideoProcessorInputView recusar a textura
    // com E_INVALIDARG na placa da AMD, quadro após quadro.
    descricao.BindFlags = 0;

    quadro.Reset();
    if (FAILED(dispositivo->CreateTexture2D(&descricao, nullptr, &quadro))) {
        erro("nao foi possivel criar a textura do quadro decodado");
        return false;
    }
    largura = novaLargura;
    altura = novaAltura;
    return true;
}

void VideoDecoder::Interno::colherSaida() {
    for (;;) {
        MFT_OUTPUT_STREAM_INFO informacao{};
        transformador->GetOutputStreamInfo(0, &informacao);

        MFT_OUTPUT_DATA_BUFFER saida{};
        DWORD estado = 0;

        // Nem todo MFT entrega a amostra pronta. O de hardware entrega; o de
        // software espera que QUEM CHAMA aloque. Sem alocar, o ProcessOutput
        // devolve sucesso com pSample nulo e nenhum quadro sai nunca - o
        // decodificador sobe, aparece no log e a tela fica preta.
        ComPtr<IMFSample> alocada;
        if (!(informacao.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            ComPtr<IMFMediaBuffer> destino;
            if (FAILED(::MFCreateMemoryBuffer(informacao.cbSize, &destino))) return;
            if (FAILED(::MFCreateSample(&alocada))) return;
            alocada->AddBuffer(destino.Get());
            saida.pSample = alocada.Get();
        }

        const HRESULT resultado = transformador->ProcessOutput(0, 1, &saida, &estado);

        if (resultado == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            saidasVazias++;
            return;
        }

        if (resultado == MF_E_TRANSFORM_STREAM_CHANGE) {
            // O decodificador leu o SPS e agora sabe a resolucao. Sem aceitar a
            // troca aqui, ele nao entrega quadro nenhum e o video fica preto
            // para sempre, sem erro.
            trocasDeTipo++;
            tipoSaidaPronto = false;
            escolherTipoSaida();
            continue;
        }

        if (FAILED(resultado)) {
            ultimoErro = resultado;
            if (saida.pEvents) saida.pEvents->Release();
            if (!alocada && saida.pSample) saida.pSample->Release();
            return;
        }

        if (saida.pEvents) saida.pEvents->Release();
        if (!saida.pSample) return;

        ComPtr<IMFSample> amostra;
        if (alocada) {
            amostra = alocada;  // a amostra e nossa: nao ha referencia extra a assumir
        } else {
            amostra.Attach(saida.pSample);
        }

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(amostra->GetBufferByIndex(0, &buffer))) continue;

        // Dois caminhos, e os dois acontecem na pratica: o decodificador por
        // hardware entrega textura na GPU, o de software entrega bytes na
        // memoria comum. Tratar so o primeiro fazia todo quadro do caminho de
        // software ser descartado em silencio - o decodificador subia, dizia o
        // nome no log, e nenhuma imagem aparecia.
        ComPtr<IMFDXGIBuffer> dxgi;
        const bool naGpu = SUCCEEDED(buffer.As(&dxgi));
        if (!avisouCaminho) {
            avisouCaminho = true;
            info("decodificacao {}", naGpu ? "na GPU" : "na CPU (sem aceleracao)");
        }
        if (naGpu) {
            ComPtr<ID3D11Texture2D> textura;
            if (FAILED(dxgi->GetResource(IID_PPV_ARGS(&textura)))) continue;

            // A textura vem de um array que o MFT reaproveita: sem o indice da
            // fatia a copia sai do quadro errado.
            UINT fatia = 0;
            dxgi->GetSubresourceIndex(&fatia);

            D3D11_TEXTURE2D_DESC descricao{};
            textura->GetDesc(&descricao);
            if (!prepararCopia(descricao.Width, descricao.Height)) continue;

            contexto->CopySubresourceRegion(quadro.Get(), 0, 0, 0, 0, textura.Get(), fatia,
                                            nullptr);
        } else {
            if (larguraSaida == 0 || alturaSaida == 0) continue;
            if (!prepararCopia(larguraSaida, alturaSaida)) continue;

            BYTE* origem = nullptr;
            DWORD tamanho = 0;
            if (FAILED(buffer->Lock(&origem, nullptr, &tamanho))) continue;

            // NV12: o plano de luminancia inteiro, depois o de cor com metade
            // das linhas. Uma escrita so cobre os dois, porque estao contiguos.
            const UINT linhas = alturaSaida + alturaSaida / 2;
            if (tamanho >= static_cast<DWORD>(larguraSaida) * linhas) {
                contexto->UpdateSubresource(quadro.Get(), 0, nullptr, origem, larguraSaida, 0);
            }
            buffer->Unlock();
        }

        quadros.fetch_add(1);
        if (consumidor) consumidor(quadro.Get(), largura, altura);
    }
}

// drenarEventos le o que o MFT assincrono tem a dizer, sem bloquear.
//
// NO_WAIT e obrigatorio: sem ele o GetEvent espera indefinidamente e trava a
// thread inteira. Foi assim que o encoder travou o processo antes.
void VideoDecoder::Interno::drenarEventos() {
    if (!assincrono || !eventos) return;

    for (;;) {
        ComPtr<IMFMediaEvent> evento;
        if (FAILED(eventos->GetEvent(MF_EVENT_FLAG_NO_WAIT, &evento)) || !evento) return;

        MediaEventType tipo = MEUnknown;
        if (FAILED(evento->GetType(&tipo))) continue;

        if (tipo == METransformNeedInput) {
            ++entradasPedidas;
        } else if (tipo == METransformHaveOutput) {
            colherSaida();
        }
    }
}

void VideoDecoder::decodificar(const uint8_t* dados, size_t tamanho, int64_t tempoUs) {
    if (!d_->iniciado || !dados || tamanho == 0) return;

    d_->drenarEventos();

    // Assincrono so aceita entrada depois de pedir. Empurrar sem pedido devolve
    // erro e o quadro se perde.
    if (d_->assincrono) {
        if (d_->entradasPedidas <= 0) return;
        --d_->entradasPedidas;
    }

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(::MFCreateMemoryBuffer(static_cast<DWORD>(tamanho), &buffer))) return;

    BYTE* destino = nullptr;
    DWORD maximo = 0;
    if (FAILED(buffer->Lock(&destino, &maximo, nullptr))) return;
    memcpy(destino, dados, tamanho);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(tamanho));

    ComPtr<IMFSample> amostra;
    if (FAILED(::MFCreateSample(&amostra))) return;
    amostra->AddBuffer(buffer.Get());
    amostra->SetSampleTime(tempoUs * 10);      // Media Foundation conta em 100ns
    amostra->SetSampleDuration(0);

    d_->entradas++;
    const HRESULT resultado = d_->transformador->ProcessInput(0, amostra.Get(), 0);
    if (resultado == MF_E_NOTACCEPTING) {
        d_->entradasRecusadas++;
        d_->colherSaida();
        d_->transformador->ProcessInput(0, amostra.Get(), 0);
    } else if (FAILED(resultado)) {
        return;
    }

    // No assincrono quem manda colher e o evento, nao nos.
    if (d_->assincrono) d_->drenarEventos();
    else d_->colherSaida();
    d_->relatar();
}

// relatar imprime o estado uma vez por segundo enquanto nada sai.
void VideoDecoder::Interno::relatar() {
    using namespace std::chrono;
    const auto agora = steady_clock::now();
    if (agora - ultimoRelato < seconds(2)) return;
    ultimoRelato = agora;

    if (quadros.load() > 0) return;  // esta saindo imagem: nao ha o que relatar

    aviso("decodificador sem saida: {} entradas, {} recusadas, {} vezes sem quadro, "
          "{} trocas de tipo, ultimo erro {}",
          entradas, entradasRecusadas, saidasVazias, trocasDeTipo, hr(ultimoErro));
}

void VideoDecoder::parar() {
    if (!d_->iniciado) return;
    d_->iniciado = false;

    d_->transformador->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    d_->transformador->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    d_->transformador.Reset();
    d_->gerenciador.Reset();
    d_->quadro.Reset();
    d_->contexto.Reset();
    d_->dispositivo.Reset();
    ::MFShutdown();
}

bool VideoDecoder::ativo() const { return d_->iniciado; }
const std::string& VideoDecoder::nome() const { return d_->nome; }
uint64_t VideoDecoder::quadrosDecodificados() const { return d_->quadros.load(); }

}  // namespace gl
