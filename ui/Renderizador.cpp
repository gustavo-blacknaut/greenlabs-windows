#include "ui/Renderizador.h"

#include <windows.h>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wincodec.h>  // WIC: decodifica o PNG da logo
#include <wrl/client.h>

#include <map>
#include <string>

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
    bool dispositivoPerdido = false;
    ComPtr<ID3D11DeviceContext> contexto;
    ComPtr<IDXGISwapChain1> cadeia;

    ComPtr<ID2D1Factory1> fabrica2d;
    ComPtr<ID2D1Device> dispositivo2d;
    ComPtr<ID2D1DeviceContext> contexto2d;
    ComPtr<ID2D1Bitmap1> alvo;
    ComPtr<ID2D1SolidColorBrush> pincel;

    ComPtr<IDWriteFactory> fabricaTexto;
    std::map<int, ComPtr<IDWriteTextFormat>> formatos;

    // Uma entrada por imagem na tela. A textura da duplicacao vem sem a flag
    // de recurso compartilhavel que o Direct2D exige, entao cada uma tem a sua
    // copia e o seu bitmap.
    struct Video {
        ComPtr<ID3D11Texture2D> copia;
        ComPtr<ID2D1Bitmap1> bitmap;
        uint32_t largura = 0;
        uint32_t altura = 0;
        D2D1_RECT_F destino{};
    };
    std::map<std::string, Video> videos;
    Video& videoDe(const std::string& chave);
    void desenhar(Video& v, const D2D1_RECT_F& area);

    ComPtr<ID2D1Bitmap1> bitmapLogo;
    bool tentouLogo = false;

    void carregarLogo();

    HWND janela = nullptr;
    uint32_t largura = 0;
    uint32_t altura = 0;

    IDWriteTextFormat* formato(Fonte f);
    bool criarAlvo();
};

Renderizador::Renderizador() : d_(std::make_unique<Interno>()) {}
Renderizador::~Renderizador() = default;

ID3D11Device* Renderizador::dispositivo() const { return d_->dispositivo.Get(); }

void Renderizador::liberar() {
    // A ordem importa: primeiro o que segura o buffer de fundo, depois a
    // cadeia. Soltar fora de ordem deixa referência pendurada e a próxima
    // criação falha.
    if (d_->contexto2d) d_->contexto2d->SetTarget(nullptr);
    d_->videos.clear();
    d_->bitmapLogo.Reset();
    d_->tentouLogo = false;
    d_->formatos.clear();
    d_->pincel.Reset();
    d_->alvo.Reset();
    d_->contexto2d.Reset();
    d_->dispositivo2d.Reset();
    d_->fabrica2d.Reset();
    d_->cadeia.Reset();

    // Limpa e esvazia o contexto antes de largar o dispositivo. Sem isto o
    // pipeline continua segurando referência ao buffer da cadeia antiga, e a
    // criação da cadeia seguinte para a MESMA janela é recusada com
    // E_ACCESSDENIED - foi o que impediu a recuperação depois do reset da GPU.
    if (d_->contexto) {
        d_->contexto->ClearState();
        d_->contexto->Flush();
    }
    d_->contexto.Reset();
    d_->dispositivo.Reset();
    d_->dispositivoPerdido = false;
}

float Renderizador::largura() const { return static_cast<float>(d_->largura); }
float Renderizador::altura() const { return static_cast<float>(d_->altura); }

bool Renderizador::iniciar(HWND janela, ID3D11Device* dispositivo) {
    liberar();
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
        // Dispositivo removido é outra história: a GPU resetou (o TDR do
        // Windows), e daí em diante TODA chamada falha. Registrar uma vez e
        // marcar; sem isso o laço voltava aqui a cada 5 ms e enchia o log com
        // milhares de linhas iguais enquanto a tela ficava parada.
        if (resultado == DXGI_ERROR_DEVICE_REMOVED || resultado == DXGI_ERROR_DEVICE_RESET) {
            if (!d_->dispositivoPerdido) {
                d_->dispositivoPerdido = true;
                HRESULT motivo = d_->dispositivo ? d_->dispositivo->GetDeviceRemovedReason() : 0;
                erro("a GPU foi reiniciada pelo Windows (motivo {})", hr(motivo));
            }
            return;
        }
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

// Carrega a logo do recurso embutido no .exe. Uma vez só: se falhar, a
// interface segue sem ela em vez de tentar a cada quadro.
void Renderizador::Interno::carregarLogo() {
    if (tentouLogo) return;
    tentouLogo = true;

    HRSRC achado = ::FindResourceW(nullptr, MAKEINTRESOURCEW(100), RT_RCDATA);
    if (!achado) return;

    HGLOBAL recurso = ::LoadResource(nullptr, achado);
    if (!recurso) return;

    const void* dados = ::LockResource(recurso);
    const DWORD tamanho = ::SizeofResource(nullptr, achado);
    if (!dados || tamanho == 0) return;

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wic)))) {
        return;
    }

    ComPtr<IWICStream> fluxo;
    if (FAILED(wic->CreateStream(&fluxo))) return;
    if (FAILED(fluxo->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<void*>(dados)), tamanho))) {
        return;
    }

    ComPtr<IWICBitmapDecoder> decodificador;
    if (FAILED(wic->CreateDecoderFromStream(fluxo.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                            &decodificador))) {
        return;
    }

    ComPtr<IWICBitmapFrameDecode> quadro;
    if (FAILED(decodificador->GetFrame(0, &quadro))) return;

    // Converte para o formato que o Direct2D usa, preservando a transparência.
    ComPtr<IWICFormatConverter> conversor;
    if (FAILED(wic->CreateFormatConverter(&conversor))) return;
    if (FAILED(conversor->Initialize(quadro.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut))) {
        return;
    }

    if (FAILED(contexto2d->CreateBitmapFromWicBitmap(conversor.Get(), nullptr, &bitmapLogo))) {
        aviso("nao foi possivel preparar a logo");
    }
}

void Renderizador::logo(const D2D1_RECT_F& area, float opacidade) {
    d_->carregarLogo();
    if (!d_->bitmapLogo) return;

    const auto tamanho = d_->bitmapLogo->GetSize();
    if (tamanho.width <= 0 || tamanho.height <= 0) return;

    const float larguraArea = area.right - area.left;
    const float alturaArea = area.bottom - area.top;
    if (larguraArea <= 0 || alturaArea <= 0) return;

    const float proporcao = tamanho.width / tamanho.height;
    float largura = larguraArea;
    float altura = largura / proporcao;
    if (altura > alturaArea) {
        altura = alturaArea;
        largura = altura * proporcao;
    }
    const float x = area.left + (larguraArea - largura) / 2.0f;
    const float y = area.top + (alturaArea - altura) / 2.0f;

    d_->contexto2d->DrawBitmap(d_->bitmapLogo.Get(), D2D1::RectF(x, y, x + largura, y + altura),
                               opacidade, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
}

void Renderizador::comecarQuadro() { d_->contexto2d->BeginDraw(); }

void Renderizador::terminarQuadro() {
    d_->contexto2d->EndDraw();
    // Intervalo 1: acompanha o refresh do monitor. Zero rasgaria a imagem e
    // gastaria GPU à toa numa interface que quase não muda.
    const HRESULT resultado = d_->cadeia->Present(1, 0);

    // O Present é onde o reset da GPU costuma aparecer primeiro. Marcar aqui
    // faz o resto do programa saber que precisa se refazer, em vez de seguir
    // chamando um dispositivo que não existe mais.
    if ((resultado == DXGI_ERROR_DEVICE_REMOVED || resultado == DXGI_ERROR_DEVICE_RESET) &&
        !d_->dispositivoPerdido) {
        d_->dispositivoPerdido = true;
        const HRESULT motivo = d_->dispositivo ? d_->dispositivo->GetDeviceRemovedReason() : 0;
        erro("a GPU foi reiniciada pelo Windows (motivo {})", hr(motivo));
    }
}

bool Renderizador::dispositivoPerdido() const { return d_->dispositivoPerdido; }

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

// Limita o desenho a um retângulo, para listas que rolam.
//
// Sem isto, uma lista maior que o painel simplesmente vazava por baixo dele e
// ia parar em cima dos botões - que é o que acontecia com a sala cheia. O
// recorte precisa ser fechado no mesmo quadro em que foi aberto: o Direct2D
// mantém uma pilha, e deixar um aberto derruba o EndDraw com erro.
void Renderizador::recortar(const D2D1_RECT_F& area) {
    d_->contexto2d->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
}

void Renderizador::soltarRecorte() { d_->contexto2d->PopAxisAlignedClip(); }

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

// Cada imagem tem a sua entrada, achada pela chave.
//
// Antes havia um cache só - uma cópia e um bitmap. Isso bastava enquanto a tela
// mostrava uma imagem por vez, e tornava impossível mostrar duas: a segunda
// sobrescrevia a primeira a cada quadro, e as duas piscavam uma na outra.
//
// Com uma entrada por chave, a prévia da própria tela e a tela de cada pessoa
// transmitindo convivem, e dá para desenhar miniatura de todas ao mesmo tempo.
Renderizador::Interno::Video& Renderizador::Interno::videoDe(const std::string& chave) {
    return videos[chave];
}

D2D1_RECT_F Renderizador::areaDoVideo(const std::string& chave) const {
    const auto achou = d_->videos.find(chave);
    return achou == d_->videos.end() ? D2D1_RECT_F{} : achou->second.destino;
}

bool Renderizador::temQuadro(const std::string& chave) const {
    const auto achou = d_->videos.find(chave);
    return achou != d_->videos.end() && achou->second.bitmap != nullptr;
}

void Renderizador::video(const std::string& chave, ID3D11Texture2D* textura,
                         const D2D1_RECT_F& area) {
    Interno::Video& v = d_->videoDe(chave);

    // Sem textura nova: repinta a última. Ver o comentário no cabeçalho.
    if (!textura) {
        if (v.bitmap) d_->desenhar(v, area);
        return;
    }

    D3D11_TEXTURE2D_DESC descricao{};
    textura->GetDesc(&descricao);
    if (descricao.Width == 0 || descricao.Height == 0) return;

    // A textura da duplicação não pode ser compartilhada com o Direct2D
    // diretamente, então vai para uma cópia criada com as flags certas. É uma
    // cópia dentro da própria GPU: nada volta para a memória principal.
    if (!v.copia || v.largura != descricao.Width || v.altura != descricao.Height) {
        v.bitmap.Reset();
        v.copia.Reset();

        D3D11_TEXTURE2D_DESC nova = descricao;
        nova.Usage = D3D11_USAGE_DEFAULT;
        nova.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        nova.CPUAccessFlags = 0;
        nova.MiscFlags = 0;
        nova.MipLevels = 1;
        nova.ArraySize = 1;

        if (FAILED(d_->dispositivo->CreateTexture2D(&nova, nullptr, &v.copia))) return;

        ComPtr<IDXGISurface> superficie;
        if (FAILED(v.copia.As(&superficie))) return;

        const auto propriedades = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(d_->contexto2d->CreateBitmapFromDxgiSurface(superficie.Get(), &propriedades,
                                                                &v.bitmap))) {
            return;
        }
        v.largura = descricao.Width;
        v.altura = descricao.Height;
    }

    d_->contexto->CopyResource(v.copia.Get(), textura);
    d_->desenhar(v, area);
}

void Renderizador::esquecerVideo(const std::string& chave) { d_->videos.erase(chave); }

void Renderizador::Interno::desenhar(Video& v, const D2D1_RECT_F& area) {
    if (!v.bitmap || v.largura == 0 || v.altura == 0) return;

    // Encaixa mantendo a proporção: esticar a tela de alguém é feio e engana
    // sobre o que está sendo transmitido.
    const float larguraArea = area.right - area.left;
    const float alturaArea = area.bottom - area.top;
    if (larguraArea <= 0 || alturaArea <= 0) return;

    const float proporcao = static_cast<float>(v.largura) / static_cast<float>(v.altura);
    float largura = larguraArea;
    float altura = largura / proporcao;
    if (altura > alturaArea) {
        altura = alturaArea;
        largura = altura * proporcao;
    }
    const float x = area.left + (larguraArea - largura) / 2.0f;
    const float y = area.top + (alturaArea - altura) / 2.0f;

    // Guarda onde a imagem REALMENTE caiu. A área pedida é o palco inteiro; a
    // imagem ocupa só o retângulo com a proporção certa dentro dela. Quem
    // desenha etiqueta em cima do vídeo precisa deste retângulo, não do palco -
    // senão a etiqueta fica boiando na faixa preta ao lado da imagem.
    v.destino = D2D1::RectF(x, y, x + largura, y + altura);

    contexto2d->DrawBitmap(v.bitmap.Get(), v.destino, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
}

}  // namespace gl
