#include "ui/Renderizador.h"

#include <windows.h>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <map>

#include "ui/Tema.h"
#include "util/Log.h"

using Microsoft::WRL::ComPtr;

namespace gl {
namespace {

const wchar_t* kFamiliaFonte = L"Segoe UI";

struct Estilo {
    float tamanho;
    DWRITE_FONT_WEIGHT peso;
};

Estilo estiloDe(Fonte f) {
    switch (f) {
        case Fonte::Titulo:    return {30.0f, DWRITE_FONT_WEIGHT_BLACK};
        case Fonte::Subtitulo: return {17.0f, DWRITE_FONT_WEIGHT_BOLD};
        case Fonte::Botao:     return {13.0f, DWRITE_FONT_WEIGHT_BOLD};
        case Fonte::Pequena:   return {11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD};
        default:               return {14.0f, DWRITE_FONT_WEIGHT_NORMAL};
    }
}

}  // namespace

struct Renderizador::Interno {
    ComPtr<ID3D11Device> dispositivo;
    ComPtr<ID3D11DeviceContext> contexto;
    ComPtr<IDXGISwapChain1> cadeia;

    ComPtr<ID2D1Factory1> fabrica2d;
    ComPtr<ID2D1Device> dispositivo2d;
    ComPtr<ID2D1DeviceContext> contexto2d;
    ComPtr<ID2D1Bitmap1> alvo;
    ComPtr<ID2D1SolidColorBrush> pincel;

    ComPtr<IDWriteFactory> fabricaTexto;
    std::map<int, ComPtr<IDWriteTextFormat>> formatos;

    // O vídeo é copiado para esta textura antes de virar bitmap do D2D: a
    // textura da duplicação vem sem a flag de recurso compartilhável que o
    // Direct2D exige.
    ComPtr<ID3D11Texture2D> copiaVideo;
    ComPtr<ID2D1Bitmap1> bitmapVideo;
    uint32_t larguraVideo = 0;
    uint32_t alturaVideo = 0;

    HWND janela = nullptr;
    uint32_t largura = 0;
    uint32_t altura = 0;

    IDWriteTextFormat* formato(Fonte f);
    bool criarAlvo();
};

Renderizador::Renderizador() : d_(std::make_unique<Interno>()) {}
Renderizador::~Renderizador() = default;

float Renderizador::largura() const { return static_cast<float>(d_->largura); }
float Renderizador::altura() const { return static_cast<float>(d_->altura); }

bool Renderizador::iniciar(HWND janela, ID3D11Device* dispositivo) {
    d_->janela = janela;
    d_->dispositivo = dispositivo;
    dispositivo->GetImmediateContext(&d_->contexto);

    ComPtr<IDXGIDevice> dxgiDispositivo;
    if (FAILED(d_->dispositivo.As(&dxgiDispositivo))) {
        erro("dispositivo D3D11 sem interface DXGI");
        return false;
    }

    ComPtr<IDXGIAdapter> adaptador;
    dxgiDispositivo->GetAdapter(&adaptador);
    ComPtr<IDXGIFactory2> fabrica;
    if (FAILED(adaptador->GetParent(IID_PPV_ARGS(&fabrica)))) {
        erro("nao foi possivel obter a fabrica DXGI");
        return false;
    }

    RECT area{};
    ::GetClientRect(janela, &area);
    d_->largura = static_cast<uint32_t>(area.right - area.left);
    d_->altura = static_cast<uint32_t>(area.bottom - area.top);

    DXGI_SWAP_CHAIN_DESC1 descricao{};
    descricao.Width = d_->largura;
    descricao.Height = d_->altura;
    descricao.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // o Direct2D exige BGRA
    descricao.SampleDesc.Count = 1;
    descricao.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    descricao.BufferCount = 2;
    // FLIP_DISCARD é o modo moderno: menos cópias e menos latência de
    // apresentação que o BitBlt antigo.
    descricao.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    descricao.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    HRESULT resultado = fabrica->CreateSwapChainForHwnd(d_->dispositivo.Get(), janela, &descricao,
                                                        nullptr, nullptr, &d_->cadeia);
    if (FAILED(resultado)) {
        erro("CreateSwapChainForHwnd falhou: {}", hr(resultado));
        return false;
    }
    // Alt+Enter do DXGI entraria em tela cheia exclusiva, que não é o que a
    // janela quer.
    fabrica->MakeWindowAssociation(janela, DXGI_MWA_NO_ALT_ENTER);

    D2D1_FACTORY_OPTIONS opcoes{};
    resultado = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                    &opcoes, reinterpret_cast<void**>(d_->fabrica2d.GetAddressOf()));
    if (FAILED(resultado)) {
        erro("D2D1CreateFactory falhou: {}", hr(resultado));
        return false;
    }
    resultado = d_->fabrica2d->CreateDevice(dxgiDispositivo.Get(), &d_->dispositivo2d);
    if (FAILED(resultado)) {
        erro("ID2D1Factory1::CreateDevice falhou: {}", hr(resultado));
        return false;
    }
    resultado = d_->dispositivo2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                        &d_->contexto2d);
    if (FAILED(resultado)) {
        erro("CreateDeviceContext falhou: {}", hr(resultado));
        return false;
    }

    resultado = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown**>(d_->fabricaTexto.GetAddressOf()));
    if (FAILED(resultado)) {
        erro("DWriteCreateFactory falhou: {}", hr(resultado));
        return false;
    }

    if (!d_->criarAlvo()) return false;

    d_->contexto2d->CreateSolidColorBrush(tema::kTexto, &d_->pincel);
    info("interface: {}x{}, Direct2D sobre a mesma GPU da captura", d_->largura, d_->altura);
    return true;
}

bool Renderizador::Interno::criarAlvo() {
    alvo.Reset();
    contexto2d->SetTarget(nullptr);

    ComPtr<IDXGISurface> superficie;
    HRESULT resultado = cadeia->GetBuffer(0, IID_PPV_ARGS(&superficie));
    if (FAILED(resultado)) return false;

    const auto propriedades = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

    resultado = contexto2d->CreateBitmapFromDxgiSurface(superficie.Get(), &propriedades, &alvo);
    if (FAILED(resultado)) {
        erro("CreateBitmapFromDxgiSurface falhou: {}", hr(resultado));
        return false;
    }
    contexto2d->SetTarget(alvo.Get());
    return true;
}

void Renderizador::redimensionar(uint32_t largura, uint32_t altura) {
    if (largura == 0 || altura == 0 || !d_->cadeia) return;
    if (largura == d_->largura && altura == d_->altura) return;

    d_->largura = largura;
    d_->altura = altura;

    // O alvo precisa soltar o buffer antes do ResizeBuffers, senão o DXGI
    // recusa por haver referência pendente.
    d_->contexto2d->SetTarget(nullptr);
    d_->alvo.Reset();

    const HRESULT resultado = d_->cadeia->ResizeBuffers(0, largura, altura, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(resultado)) {
        erro("ResizeBuffers falhou: {}", hr(resultado));
        return;
    }
    d_->criarAlvo();
}

IDWriteTextFormat* Renderizador::Interno::formato(Fonte f) {
    const int chave = static_cast<int>(f);
    auto achou = formatos.find(chave);
    if (achou != formatos.end()) return achou->second.Get();

    const Estilo estilo = estiloDe(f);
    ComPtr<IDWriteTextFormat> novo;
    if (FAILED(fabricaTexto->CreateTextFormat(kFamiliaFonte, nullptr, estilo.peso,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              estilo.tamanho, L"pt-br", &novo))) {
        return nullptr;
    }
    novo->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    novo->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    formatos[chave] = novo;
    return novo.Get();
}

void Renderizador::comecarQuadro() { d_->contexto2d->BeginDraw(); }

void Renderizador::terminarQuadro() {
    d_->contexto2d->EndDraw();
    // Intervalo 1: acompanha o refresh do monitor. Zero rasgaria a imagem e
    // gastaria GPU à toa numa interface que quase não muda.
    d_->cadeia->Present(1, 0);
}

void Renderizador::limpar(const D2D1_COLOR_F& cor) { d_->contexto2d->Clear(cor); }

void Renderizador::retangulo(const D2D1_RECT_F& area, const D2D1_COLOR_F& cor, float raio) {
    if (cor.a <= 0.0f) return;
    d_->pincel->SetColor(cor);
    if (raio > 0.0f) {
        d_->contexto2d->FillRoundedRectangle(D2D1::RoundedRect(area, raio, raio), d_->pincel.Get());
    } else {
        d_->contexto2d->FillRectangle(area, d_->pincel.Get());
    }
}

void Renderizador::contorno(const D2D1_RECT_F& area, const D2D1_COLOR_F& cor, float raio,
                            float espessura) {
    if (cor.a <= 0.0f) return;
    d_->pincel->SetColor(cor);
    if (raio > 0.0f) {
        d_->contexto2d->DrawRoundedRectangle(D2D1::RoundedRect(area, raio, raio), d_->pincel.Get(),
                                             espessura);
    } else {
        d_->contexto2d->DrawRectangle(area, d_->pincel.Get(), espessura);
    }
}

void Renderizador::linha(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& cor,
                         float espessura) {
    d_->pincel->SetColor(cor);
    d_->contexto2d->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), d_->pincel.Get(),
                             espessura);
}

void Renderizador::texto(const std::wstring& conteudo, const D2D1_RECT_F& area,
                         const D2D1_COLOR_F& cor, Fonte fonte,
                         DWRITE_TEXT_ALIGNMENT alinhamento) {
    IDWriteTextFormat* f = d_->formato(fonte);
    if (!f || conteudo.empty()) return;
    f->SetTextAlignment(alinhamento);
    d_->pincel->SetColor(cor);
    d_->contexto2d->DrawTextW(conteudo.c_str(), static_cast<UINT32>(conteudo.size()), f, area,
                              d_->pincel.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Renderizador::larguraDoTexto(const std::wstring& conteudo, Fonte fonte) {
    IDWriteTextFormat* f = d_->formato(fonte);
    if (!f || conteudo.empty()) return 0.0f;

    ComPtr<IDWriteTextLayout> arranjo;
    if (FAILED(d_->fabricaTexto->CreateTextLayout(conteudo.c_str(),
                                                  static_cast<UINT32>(conteudo.size()), f, 4096.0f,
                                                  64.0f, &arranjo))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS medidas{};
    arranjo->GetMetrics(&medidas);
    return medidas.widthIncludingTrailingWhitespace;
}

bool Renderizador::temQuadro() const { return d_->bitmapVideo != nullptr; }

void Renderizador::video(ID3D11Texture2D* textura, const D2D1_RECT_F& area) {
    // Sem textura nova: repinta a última. Ver o comentário no cabeçalho.
    if (!textura) {
        if (d_->bitmapVideo) desenharUltimo(area);
        return;
    }

    D3D11_TEXTURE2D_DESC descricao{};
    textura->GetDesc(&descricao);
    if (descricao.Width == 0 || descricao.Height == 0) return;

    // A textura da duplicação não pode ser compartilhada com o Direct2D
    // diretamente, então vai para uma cópia criada com as flags certas. É uma
    // cópia dentro da própria GPU: nada volta para a memória principal.
    if (!d_->copiaVideo || d_->larguraVideo != descricao.Width ||
        d_->alturaVideo != descricao.Height) {
        d_->bitmapVideo.Reset();
        d_->copiaVideo.Reset();

        D3D11_TEXTURE2D_DESC nova = descricao;
        nova.Usage = D3D11_USAGE_DEFAULT;
        nova.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        nova.CPUAccessFlags = 0;
        nova.MiscFlags = 0;
        nova.MipLevels = 1;
        nova.ArraySize = 1;

        if (FAILED(d_->dispositivo->CreateTexture2D(&nova, nullptr, &d_->copiaVideo))) return;

        ComPtr<IDXGISurface> superficie;
        if (FAILED(d_->copiaVideo.As(&superficie))) return;

        const auto propriedades = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(d_->contexto2d->CreateBitmapFromDxgiSurface(superficie.Get(), &propriedades,
                                                                &d_->bitmapVideo))) {
            return;
        }
        d_->larguraVideo = descricao.Width;
        d_->alturaVideo = descricao.Height;
    }

    d_->contexto->CopyResource(d_->copiaVideo.Get(), textura);
    desenharUltimo(area);
}

void Renderizador::desenharUltimo(const D2D1_RECT_F& area) {
    if (!d_->bitmapVideo || d_->larguraVideo == 0 || d_->alturaVideo == 0) return;

    // Encaixa mantendo a proporção: esticar a tela de alguém é feio e engana
    // sobre o que está sendo transmitido.
    const float larguraArea = area.right - area.left;
    const float alturaArea = area.bottom - area.top;
    if (larguraArea <= 0 || alturaArea <= 0) return;

    const float proporcao =
        static_cast<float>(d_->larguraVideo) / static_cast<float>(d_->alturaVideo);
    float largura = larguraArea;
    float altura = largura / proporcao;
    if (altura > alturaArea) {
        altura = alturaArea;
        largura = altura * proporcao;
    }
    const float x = area.left + (larguraArea - largura) / 2.0f;
    const float y = area.top + (alturaArea - altura) / 2.0f;

    d_->contexto2d->DrawBitmap(d_->bitmapVideo.Get(), D2D1::RectF(x, y, x + largura, y + altura),
                               1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
}

}  // namespace gl
