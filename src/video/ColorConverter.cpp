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
};

ColorConverter::ColorConverter() : d_(std::make_unique<Interno>()) {}
ColorConverter::~ColorConverter() = default;

bool ColorConverter::iniciar(ID3D11Device* dispositivo, ID3D11DeviceContext* contexto,
                             uint32_t larguraEntrada, uint32_t alturaEntrada,
                             uint32_t larguraSaida, uint32_t alturaSaida,
                             Saida formatoSaida) {
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

    info("conversao BGRA {}x{} -> NV12 {}x{} na GPU", larguraEntrada, alturaEntrada, d_->largura,
         d_->altura);
    return true;
}

ID3D11Texture2D* ColorConverter::converter(ID3D11Texture2D* entradaBgra) {
    if (!d_->processador || !entradaBgra) return nullptr;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC descricao{};
    descricao.FourCC = 0;
    descricao.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    descricao.Texture2D.MipSlice = 0;
    descricao.Texture2D.ArraySlice = 0;

    ComPtr<ID3D11VideoProcessorInputView> vistaEntrada;
    HRESULT resultado = d_->videoDevice->CreateVideoProcessorInputView(
        entradaBgra, d_->enumerador.Get(), &descricao, &vistaEntrada);
    if (FAILED(resultado)) {
        erro("CreateVideoProcessorInputView falhou: {}", hr(resultado));
        return nullptr;
    }

    D3D11_VIDEO_PROCESSOR_STREAM fluxo{};
    fluxo.Enable = TRUE;
    fluxo.pInputSurface = vistaEntrada.Get();

    const size_t indice = d_->proximaSaida;
    d_->proximaSaida = (d_->proximaSaida + 1) % Interno::kSaidas;

    resultado = d_->videoContexto->VideoProcessorBlt(
        d_->processador.Get(), d_->vistaSaida[indice].Get(), 0, 1, &fluxo);
    if (FAILED(resultado)) {
        erro("VideoProcessorBlt falhou: {}", hr(resultado));
        return nullptr;
    }
    return d_->saidaNv12[indice].Get();
}

uint32_t ColorConverter::largura() const { return d_->largura; }
uint32_t ColorConverter::altura() const { return d_->altura; }

}  // namespace gl
