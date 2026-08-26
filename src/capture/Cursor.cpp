#include "capture/Cursor.h"

#include <windows.h>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "util/Log.h"

using Microsoft::WRL::ComPtr;

namespace gl {

struct Cursor::Interno {
    ComPtr<ID3D11Device> dispositivo;
    ComPtr<ID3D11DeviceContext> contexto;

    // Cópia do quadro onde o cursor é desenhado. A textura da duplicação não
    // pode ser usada como alvo de desenho, e escrever nela seria errado de
    // qualquer forma: ela pertence ao DXGI.
    ComPtr<ID3D11Texture2D> composto;
    ComPtr<ID2D1Factory1> fabrica;
    ComPtr<ID2D1Device> dispositivo2d;
    ComPtr<ID2D1DeviceContext> contexto2d;
    ComPtr<ID2D1Bitmap1> alvo;
    ComPtr<ID2D1Bitmap> bitmapCursor;

    uint32_t largura = 0;
    uint32_t altura = 0;
    int32_t ancoraX = 0;
    int32_t ancoraY = 0;
    uint32_t larguraCursor = 0;
    uint32_t alturaCursor = 0;
};

Cursor::Cursor() : d_(std::make_unique<Interno>()) {}
Cursor::~Cursor() = default;

bool Cursor::iniciar(ID3D11Device* dispositivo, ID3D11DeviceContext* contexto, uint32_t largura,
                     uint32_t altura) {
    d_->dispositivo = dispositivo;
    d_->contexto = contexto;
    d_->largura = largura;
    d_->altura = altura;

    D3D11_TEXTURE2D_DESC descricao{};
    descricao.Width = largura;
    descricao.Height = altura;
    descricao.MipLevels = 1;
    descricao.ArraySize = 1;
    descricao.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    descricao.SampleDesc.Count = 1;
    descricao.Usage = D3D11_USAGE_DEFAULT;
    descricao.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT resultado = d_->dispositivo->CreateTexture2D(&descricao, nullptr, &d_->composto);
    if (FAILED(resultado)) {
        erro("cursor: nao foi possivel criar a textura de composicao: {}", hr(resultado));
        return false;
    }

    D2D1_FACTORY_OPTIONS opcoes{};
    resultado = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                    &opcoes, reinterpret_cast<void**>(d_->fabrica.GetAddressOf()));
    if (FAILED(resultado)) return false;

    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(d_->dispositivo.As(&dxgi))) return false;
    if (FAILED(d_->fabrica->CreateDevice(dxgi.Get(), &d_->dispositivo2d))) return false;
    if (FAILED(d_->dispositivo2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                       &d_->contexto2d))) {
        return false;
    }

    ComPtr<IDXGISurface> superficie;
    if (FAILED(d_->composto.As(&superficie))) return false;

    const auto propriedades = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(d_->contexto2d->CreateBitmapFromDxgiSurface(superficie.Get(), &propriedades,
                                                            &d_->alvo))) {
        return false;
    }
    d_->contexto2d->SetTarget(d_->alvo.Get());
    return true;
}

void Cursor::definirForma(const FormaCursor& forma) {
    d_->bitmapCursor.Reset();
    if (!forma.valida() || !d_->contexto2d) return;

    // O Direct2D quer alfa pré-multiplicado; o cursor vem com alfa direto.
    std::vector<uint8_t> premultiplicado = forma.pixels;
    for (size_t i = 0; i + 3 < premultiplicado.size(); i += 4) {
        const uint32_t a = premultiplicado[i + 3];
        premultiplicado[i + 0] = static_cast<uint8_t>(premultiplicado[i + 0] * a / 255);
        premultiplicado[i + 1] = static_cast<uint8_t>(premultiplicado[i + 1] * a / 255);
        premultiplicado[i + 2] = static_cast<uint8_t>(premultiplicado[i + 2] * a / 255);
    }

    const auto propriedades = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    if (FAILED(d_->contexto2d->CreateBitmap(D2D1::SizeU(forma.largura, forma.altura),
                                            premultiplicado.data(), forma.largura * 4,
                                            propriedades, &d_->bitmapCursor))) {
        return;
    }
    d_->larguraCursor = forma.largura;
    d_->alturaCursor = forma.altura;
    d_->ancoraX = forma.ancoraX;
    d_->ancoraY = forma.ancoraY;
}

ID3D11Texture2D* Cursor::compor(ID3D11Texture2D* quadro, bool visivel, int32_t x, int32_t y) {
    // Sem cursor para desenhar, devolve o próprio quadro: assim o caminho sem
    // ponteiro continua sem nenhuma cópia extra.
    if (!quadro || !visivel || !d_->bitmapCursor || !d_->composto) return quadro;

    d_->contexto->CopyResource(d_->composto.Get(), quadro);

    const float px = static_cast<float>(x - d_->ancoraX);
    const float py = static_cast<float>(y - d_->ancoraY);

    d_->contexto2d->BeginDraw();
    d_->contexto2d->DrawBitmap(
        d_->bitmapCursor.Get(),
        D2D1::RectF(px, py, px + static_cast<float>(d_->larguraCursor),
                    py + static_cast<float>(d_->alturaCursor)),
        1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    d_->contexto2d->EndDraw();

    return d_->composto.Get();
}

}  // namespace gl
