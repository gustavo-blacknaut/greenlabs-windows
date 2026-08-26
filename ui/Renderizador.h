#pragma once

// Desenho da interface: Direct3D 11 para o vídeo, Direct2D e DirectWrite para
// o resto.
//
// Todos vêm no Windows SDK, então a interface não custa uma dependência nova —
// a mesma regra que vale para a captura e o encoder. E o vídeo capturado já é
// uma textura D3D11: pintá-la aqui é uma cópia dentro da própria GPU, sem
// passar pela memória principal.

#include <d2d1_1.h>
#include <dwrite.h>

#include <cstdint>
#include <memory>
#include <string>

struct ID3D11Device;
struct ID3D11Texture2D;
struct HWND__;
using HWND = HWND__*;

namespace gl {

enum class Fonte { Titulo, Subtitulo, Corpo, Pequena, Botao };

class Renderizador {
public:
    Renderizador();
    ~Renderizador();

    Renderizador(const Renderizador&) = delete;
    Renderizador& operator=(const Renderizador&) = delete;

    bool iniciar(HWND janela, ID3D11Device* dispositivo);

    // Solta tudo que depende do dispositivo D3D11 e da janela.
    //
    // Precisa ser chamado ANTES de destruir o dispositivo da captura: o DXGI
    // permite uma cadeia de troca por janela, e recriar sem soltar a anterior
    // falha em silêncio. Pior, a cadeia continuaria apontando para um
    // dispositivo que já morreu.
    void liberar();

    // Qual dispositivo está em uso, para quem chama saber se precisa recriar.
    ID3D11Device* dispositivo() const;
    void redimensionar(uint32_t largura, uint32_t altura);

    void comecarQuadro();
    void terminarQuadro();

    float largura() const;
    float altura() const;

    void limpar(const D2D1_COLOR_F& cor);
    void retangulo(const D2D1_RECT_F& area, const D2D1_COLOR_F& cor, float raio = 0.0f);
    void contorno(const D2D1_RECT_F& area, const D2D1_COLOR_F& cor, float raio = 0.0f,
                  float espessura = 1.0f);
    void linha(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& cor,
               float espessura = 1.0f);

    void texto(const std::wstring& conteudo, const D2D1_RECT_F& area, const D2D1_COLOR_F& cor,
               Fonte fonte = Fonte::Corpo, DWRITE_TEXT_ALIGNMENT alinhamento = DWRITE_TEXT_ALIGNMENT_LEADING);
    float larguraDoTexto(const std::wstring& conteudo, Fonte fonte);

    // Desenha uma textura de vídeo encaixada na área, mantendo a proporção.
    // A textura precisa ser do mesmo dispositivo D3D11.
    //
    // Passar nullptr redesenha o último quadro recebido. A duplicação de área de
    // trabalho só entrega quadro quando a tela muda, então na maior parte dos
    // instantes não há nada novo — e apagar a imagem nesses instantes fazia a
    // prévia piscar sem parar.
    void video(ID3D11Texture2D* textura, const D2D1_RECT_F& area);

    // Houve algum quadro desde que a captura começou.
    bool temQuadro() const;

    // Desenha a logo, que vem embutida no próprio executável. Encaixa na área
    // mantendo a proporção.
    void logo(const D2D1_RECT_F& area, float opacidade = 1.0f);

private:
    void desenharUltimo(const D2D1_RECT_F& area);

    struct Interno;
    std::unique_ptr<Interno> d_;
};

}  // namespace gl
