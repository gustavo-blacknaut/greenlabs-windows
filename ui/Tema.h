#pragma once

// Paleta e medidas da interface.
//
// Os valores vieram do :root do cliente em Electron, copiados um a um, para o
// aplicativo nativo parecer o mesmo produto e não um primo distante.

#include <d2d1.h>

namespace gl::tema {

// D2D trabalha com componentes de 0 a 1; os comentários guardam o valor
// original em hexadecimal, que é como eles aparecem no CSS.
constexpr D2D1_COLOR_F cor(uint32_t rgb, float alfa = 1.0f) {
    return D2D1_COLOR_F{
        static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
        static_cast<float>(rgb & 0xFF) / 255.0f,
        alfa,
    };
}

inline constexpr D2D1_COLOR_F kFundo       = cor(0x05070A);          // --ink
inline constexpr D2D1_COLOR_F kTexto       = cor(0xF6FFF9);
inline constexpr D2D1_COLOR_F kVerde       = cor(0x37FF94);          // --green
inline constexpr D2D1_COLOR_F kVerdeSuave  = cor(0x37FF94, 0.14f);   // --green-soft
inline constexpr D2D1_COLOR_F kVerdeLinha  = cor(0x37FF94, 0.35f);   // --green-line
inline constexpr D2D1_COLOR_F kAzul        = cor(0x53A8FF);          // --blue
inline constexpr D2D1_COLOR_F kPainel      = cor(0x0C1018, 0.86f);   // --panel
inline constexpr D2D1_COLOR_F kPainel2     = cor(0xFFFFFF, 0.045f);  // --panel-2
// Um degrau acima do kPainel2, para o que está sob o ponteiro. A diferença é
// pequena de propósito: realce forte em cada linha apontada faz o painel
// piscar enquanto a mão passa por cima dele.
inline constexpr D2D1_COLOR_F kPainel3     = cor(0xFFFFFF, 0.085f);  // --panel-hover
inline constexpr D2D1_COLOR_F kLinha       = cor(0xFFFFFF, 0.08f);   // --line
inline constexpr D2D1_COLOR_F kApagado     = cor(0x93A3B3);          // --muted
inline constexpr D2D1_COLOR_F kVermelho    = cor(0xFF6B6B);
// O amarelo do aviso do servidor, o mesmo do .aviso-servidor do Electron.
inline constexpr D2D1_COLOR_F kAmarelo     = cor(0xFFC857);

// O modal e o que fica atras dele.
//
// O Electron escurece a janela inteira com rgba(2,4,7,0.85) e desfoca o fundo.
// Desfocar aqui custaria um passe de shader por quadro; escurecer sozinho ja
// diz "resolva isto primeiro", que e o unico trabalho da camada.
inline constexpr D2D1_COLOR_F kSombraModal = cor(0x020407, 0.85f);
inline constexpr D2D1_COLOR_F kFundoModal  = cor(0x0D1119);          // .picker-modal
// Uma linha mais visivel, para a borda do que esta sob o ponteiro.
inline constexpr D2D1_COLOR_F kLinhaForte  = cor(0xFFFFFF, 0.18f);
// O verde do botao principal em repouso; o kVerde puro fica para o realce.
inline constexpr D2D1_COLOR_F kVerdeForte  = cor(0x2BE283);
inline constexpr D2D1_COLOR_F kTransparente = cor(0x000000, 0.0f);

// Barra de título sem moldura, do mesmo tamanho da do Electron.
inline constexpr float kAlturaTitulo = 38.0f;
inline constexpr float kLarguraBotaoTitulo = 46.0f;

inline constexpr float kRaioCartao = 18.0f;
inline constexpr float kRaioBotao = 12.0f;
inline constexpr float kEspaco = 14.0f;
inline constexpr float kLarguraPainelLateral = 300.0f;

}  // namespace gl::tema
