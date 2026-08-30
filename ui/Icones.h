#pragma once

// Ícones desenhados, não escritos.
//
// A primeira versão usava caracteres — ⚙, ✕, ▣, ✓. Depender de glifo é depender
// de a fonte instalada ter aquele ponto de código: quando não tem, o Windows
// desenha o retângulo vazio, e foi o que apareceu no lugar da engrenagem. Os
// que existem também não ajudam muito, porque cada fonte desenha o seu com um
// peso e um tamanho diferentes, e ao lado de um ícone desenhado eles ficam
// grandes demais ou tortos.
//
// Traço, raio e proporção iguais em todos: são duas linhas, um retângulo e um
// círculo cada. O Direct2D já desenha os três com antialiasing, e nada disso
// depende de fonte nenhuma.
//
// Todos recebem o RETÂNGULO em que devem caber e se centram nele, para quem
// chama não precisar fazer conta.

#include <d2d1.h>

#include <cmath>

#include "ui/Renderizador.h"

namespace gl::icone {

// O tamanho de desenho dentro da área recebida. Os ícones da interface são
// pequenos; deixar folga em volta é o que os faz parecer alinhados com o texto.
inline D2D1_RECT_F encaixar(const D2D1_RECT_F& area, float lado) {
    const float cx = (area.left + area.right) / 2;
    const float cy = (area.top + area.bottom) / 2;
    const float meio = lado / 2;
    return D2D1::RectF(cx - meio, cy - meio, cx + meio, cy + meio);
}

/// Um X, para fechar.
inline void fechar(Renderizador& r, const D2D1_RECT_F& area, const D2D1_COLOR_F& cor,
                   float lado = 11.0f, float traco = 1.6f) {
    const auto a = encaixar(area, lado);
    r.linha(a.left, a.top, a.right, a.bottom, cor, traco);
    r.linha(a.right, a.top, a.left, a.bottom, cor, traco);
}

/// Um traço, para minimizar.
inline void minimizar(Renderizador& r, const D2D1_RECT_F& area, const D2D1_COLOR_F& cor,
                      float lado = 11.0f, float traco = 1.6f) {
    const auto a = encaixar(area, lado);
    const float meio = (a.top + a.bottom) / 2;
    r.linha(a.left, meio, a.right, meio, cor, traco);
}

/// Um quadrado, para maximizar.
inline void maximizar(Renderizador& r, const D2D1_RECT_F& area, const D2D1_COLOR_F& cor,
                      float lado = 10.0f, float traco = 1.4f) {
    r.contorno(encaixar(area, lado), cor, 2, traco);
}

/// Dois quadrados sobrepostos, para restaurar o tamanho.
inline void restaurar(Renderizador& r, const D2D1_RECT_F& area, const D2D1_COLOR_F& cor,
                      float lado = 10.0f, float traco = 1.4f) {
    const auto a = encaixar(area, lado);
    r.contorno(D2D1::RectF(a.left, a.top + 3, a.right - 3, a.bottom), cor, 2, traco);
    r.contorno(D2D1::RectF(a.left + 3, a.top, a.right, a.bottom - 3), cor, 2, traco);
}

/// Um visto, para o que está escolhido.
inline void visto(Renderizador& r, const D2D1_RECT_F& area, const D2D1_COLOR_F& cor,
                  float lado = 11.0f, float traco = 2.0f) {
    const auto a = encaixar(area, lado);
    const float meioX = a.left + (a.right - a.left) * 0.40f;
    const float baixo = a.bottom - 1;
    r.linha(a.left, a.top + (a.bottom - a.top) * 0.55f, meioX, baixo, cor, traco);
    r.linha(meioX, baixo, a.right, a.top + 1, cor, traco);
}

/// Um monitor: retângulo com pezinho.
inline void monitor(Renderizador& r, const D2D1_RECT_F& area, const D2D1_COLOR_F& cor,
                    float lado = 14.0f, float traco = 1.4f) {
    const auto a = encaixar(area, lado);
    const float altura = (a.bottom - a.top) * 0.72f;
    r.contorno(D2D1::RectF(a.left, a.top, a.right, a.top + altura), cor, 2, traco);
    const float meio = (a.left + a.right) / 2;
    r.linha(meio, a.top + altura, meio, a.bottom, cor, traco);
    r.linha(meio - 3, a.bottom, meio + 3, a.bottom, cor, traco);
}

/// Duas pessoas: dois círculos, um atrás do outro.
inline void pessoas(Renderizador& r, const D2D1_RECT_F& area, const D2D1_COLOR_F& cor,
                    float lado = 14.0f, float traco = 1.4f) {
    const auto a = encaixar(area, lado);
    const float d = (a.bottom - a.top) * 0.62f;
    r.contorno(D2D1::RectF(a.left, a.top + 1, a.left + d, a.top + 1 + d), cor, d / 2, traco);
    r.contorno(D2D1::RectF(a.right - d, a.top + 3, a.right, a.top + 3 + d), cor, d / 2, traco);
}

/// Uma câmera: retângulo com o bico apontando para a direita.
inline void camera(Renderizador& r, const D2D1_RECT_F& area, const D2D1_COLOR_F& cor,
                   float lado = 14.0f, float traco = 1.4f) {
    const auto a = encaixar(area, lado);
    const float altura = (a.bottom - a.top) * 0.62f;
    const float topo = a.top + ((a.bottom - a.top) - altura) / 2;
    const float corpoDireita = a.left + (a.right - a.left) * 0.66f;
    r.contorno(D2D1::RectF(a.left, topo, corpoDireita, topo + altura), cor, 2, traco);
    r.linha(corpoDireita + 1, topo + altura * 0.35f, a.right, topo, cor, traco);
    r.linha(corpoDireita + 1, topo + altura * 0.65f, a.right, topo + altura, cor, traco);
    r.linha(a.right, topo, a.right, topo + altura, cor, traco);
}

/// Uma engrenagem: um anel com oito dentes curtos para fora.
///
/// A primeira versão punha quatro bolinhas nos quatro lados do anel, e o
/// resultado lia como losango, não como engrenagem - as bolinhas ficavam fora
/// do anel e eram a única coisa que se via. Dentes são traços curtos saindo da
/// borda, e oito bastam para o olho fechar a forma nesse tamanho.
inline void engrenagem(Renderizador& r, const D2D1_RECT_F& area, const D2D1_COLOR_F& cor,
                       float lado = 16.0f, float traco = 1.5f) {
    const auto a = encaixar(area, lado);
    const float cx = (a.left + a.right) / 2;
    const float cy = (a.top + a.bottom) / 2;
    const float raio = (a.right - a.left) / 2;

    const float anel = raio * 0.60f;
    r.contorno(D2D1::RectF(cx - anel, cy - anel, cx + anel, cy + anel), cor, anel, traco);

    // Oito dentes, do anel até a borda.
    for (int i = 0; i < 8; ++i) {
        const float ang = static_cast<float>(i) * 0.7853981634f;  // 45 graus
        const float sx = cx + std::cos(ang) * anel;
        const float sy = cy + std::sin(ang) * anel;
        const float ex = cx + std::cos(ang) * raio;
        const float ey = cy + std::sin(ang) * raio;
        r.linha(sx, sy, ex, ey, cor, traco + 0.4f);
    }

    // O furo do meio, para não virar um sol.
    const float furo = raio * 0.24f;
    r.contorno(D2D1::RectF(cx - furo, cy - furo, cx + furo, cy + furo), cor, furo, traco);
}

/// Uma porta com a seta saindo, para sair da sala.
inline void porta(Renderizador& r, const D2D1_RECT_F& area, const D2D1_COLOR_F& cor,
                  float lado = 15.0f, float traco = 1.5f) {
    const auto a = encaixar(area, lado);
    const float meio = (a.top + a.bottom) / 2;

    // A porta: três lados, aberta para a direita, por onde a seta sai.
    const float batenteDireita = a.left + (a.right - a.left) * 0.52f;
    r.linha(batenteDireita, a.top, a.left, a.top, cor, traco);
    r.linha(a.left, a.top, a.left, a.bottom, cor, traco);
    r.linha(a.left, a.bottom, batenteDireita, a.bottom, cor, traco);

    // A seta saindo.
    const float pontaX = a.right;
    const float caudaX = a.left + (a.right - a.left) * 0.42f;
    r.linha(caudaX, meio, pontaX, meio, cor, traco);
    r.linha(pontaX - 4, meio - 4, pontaX, meio, cor, traco);
    r.linha(pontaX - 4, meio + 4, pontaX, meio, cor, traco);
}

/// Um monitor com ondas saindo, para transmitir.
inline void transmitir(Renderizador& r, const D2D1_RECT_F& area, const D2D1_COLOR_F& cor,
                       float lado = 16.0f, float traco = 1.5f) {
    const auto a = encaixar(area, lado);
    const float largura = (a.right - a.left) * 0.62f;
    const float altura = (a.bottom - a.top) * 0.68f;
    const float topo = a.top + ((a.bottom - a.top) - altura) / 2;

    r.contorno(D2D1::RectF(a.left, topo, a.left + largura, topo + altura), cor, 2, traco);
    const float meioX = a.left + largura / 2;
    r.linha(meioX, topo + altura, meioX, a.bottom, cor, traco);

    // Duas ondas à direita, como as de sinal.
    const float cy = topo + altura / 2;
    for (int i = 0; i < 2; ++i) {
        const float x = a.left + largura + 2 + static_cast<float>(i) * 3.5f;
        const float h = 2.5f + static_cast<float>(i) * 2.5f;
        r.linha(x, cy - h, x + 1.5f, cy, cor, traco);
        r.linha(x + 1.5f, cy, x, cy + h, cor, traco);
    }
}

/// Um quadrado cheio, para parar.
inline void parar(Renderizador& r, const D2D1_RECT_F& area, const D2D1_COLOR_F& cor,
                  float lado = 11.0f) {
    r.retangulo(encaixar(area, lado), cor, 2);
}

/// Um ponto cheio, para "ao vivo".
inline void aoVivo(Renderizador& r, const D2D1_RECT_F& area, const D2D1_COLOR_F& cor,
                   float lado = 7.0f) {
    r.retangulo(encaixar(area, lado), cor, lado / 2);
}

}  // namespace gl::icone
