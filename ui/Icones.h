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

/// Uma engrenagem: um anel com quatro dentes e o furo no meio.
///
/// Dente redondo em vez de quadrado: a esta altura, quatro retângulos de dois
/// pixels em volta de um círculo leem como sujeira, e o círculo lê como dente.
inline void engrenagem(Renderizador& r, const D2D1_RECT_F& area, const D2D1_COLOR_F& cor,
                       float lado = 15.0f, float traco = 1.5f) {
    const auto a = encaixar(area, lado);
    const float cx = (a.left + a.right) / 2;
    const float cy = (a.top + a.bottom) / 2;
    const float raio = (a.right - a.left) / 2;

    const float dente = raio * 0.30f;
    for (int i = 0; i < 4; ++i) {
        const float dx = (i == 0) ? -raio : (i == 1) ? raio : 0.0f;
        const float dy = (i == 2) ? -raio : (i == 3) ? raio : 0.0f;
        r.retangulo(D2D1::RectF(cx + dx - dente, cy + dy - dente, cx + dx + dente, cy + dy + dente),
                    cor, dente);
    }

    const float anel = raio * 0.74f;
    r.contorno(D2D1::RectF(cx - anel, cy - anel, cx + anel, cy + anel), cor, anel, traco + 0.6f);
}

/// Um ponto cheio, para "ao vivo".
inline void aoVivo(Renderizador& r, const D2D1_RECT_F& area, const D2D1_COLOR_F& cor,
                   float lado = 7.0f) {
    r.retangulo(encaixar(area, lado), cor, lado / 2);
}

}  // namespace gl::icone
