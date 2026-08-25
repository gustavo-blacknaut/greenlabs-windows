#pragma once

// Buffer circular entre a thread de áudio e quem consome.
//
// Porte de public/wasapi-audio-worklet.js. Os números aqui vieram de medição,
// não de bom senso, e mudá-los quebra o áudio de um jeito difícil de notar:
//
//   O áudio não chega gota a gota, chega em rajadas — a thread do WASAPI acorda
//   com um pacote inteiro. Um teto fixo de 40 ms contra rajadas de 60 ms joga
//   fora a maior parte de cada rajada assim que ela entra. Medido no cliente em
//   Electron: só 66% do áudio era tocado, o resto saía como silêncio.
//
//   Por isso o teto tem piso de 40 ms — mantido baixo para entrega regular
//   continuar com latência baixa — mas cresce até o dobro da maior rajada já
//   vista. Quando a entrega é regular isso não custa nada, e quando não é,
//   evita o descarte.
//
// Um produtor e um consumidor apenas. Sem trava: a thread de áudio nunca
// espera por ninguém.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gl {

class RingBuffer {
public:
    RingBuffer(uint32_t taxaAmostragem, uint32_t canais)
        : canais_(canais),
          capacidade_(static_cast<size_t>(std::ceil(taxaAmostragem * 0.3)) * canais),
          tetoBase_(static_cast<size_t>(taxaAmostragem * 0.04) * canais),
          teto_(tetoBase_),
          dados_(capacidade_, 0.0f) {}

    // Chamado da thread de áudio. Nunca bloqueia.
    void escrever(const float* intercalado, uint32_t quadros) {
        const size_t amostras = static_cast<size_t>(quadros) * canais_;
        if (amostras == 0) return;

        if (amostras > maiorRajada_) {
            maiorRajada_ = amostras;
            teto_ = std::min(capacidade_, std::max(tetoBase_, amostras * 2));
        }

        for (size_t i = 0; i < amostras; ++i) {
            dados_[escrita_] = intercalado[i];
            escrita_ = (escrita_ + 1) % capacidade_;
            const size_t atual = ocupacao_.load(std::memory_order_relaxed);
            if (atual < capacidade_) {
                ocupacao_.store(atual + 1, std::memory_order_release);
            } else {
                leitura_ = (leitura_ + 1) % capacidade_;
            }
        }

        // Descarta o excedente pelo lado antigo: em atraso, o que importa é o
        // som de agora, não o de meio segundo atrás.
        while (ocupacao_.load(std::memory_order_relaxed) > teto_) {
            leitura_ = (leitura_ + 1) % capacidade_;
            ocupacao_.fetch_sub(1, std::memory_order_release);
            descartadas_ += 1;
        }
    }

    // Preenche com o que houver e completa com silêncio. Devolve quantas
    // amostras eram de verdade.
    size_t ler(float* saida, size_t amostras) {
        size_t lidas = 0;
        for (size_t i = 0; i < amostras; ++i) {
            if (ocupacao_.load(std::memory_order_acquire) > 0) {
                saida[i] = dados_[leitura_];
                leitura_ = (leitura_ + 1) % capacidade_;
                ocupacao_.fetch_sub(1, std::memory_order_release);
                ++lidas;
            } else {
                saida[i] = 0.0f;
            }
        }
        return lidas;
    }

    size_t ocupacao() const { return ocupacao_.load(std::memory_order_acquire); }
    size_t teto() const { return teto_; }
    size_t maiorRajada() const { return maiorRajada_; }
    uint64_t descartadas() const { return descartadas_; }

private:
    uint32_t canais_;
    size_t capacidade_;
    size_t tetoBase_;
    size_t teto_;
    size_t maiorRajada_ = 0;
    uint64_t descartadas_ = 0;

    std::vector<float> dados_;
    size_t escrita_ = 0;
    size_t leitura_ = 0;
    std::atomic<size_t> ocupacao_{0};
};

}  // namespace gl
