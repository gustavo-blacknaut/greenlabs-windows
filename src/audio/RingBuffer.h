#pragma once

// Buffer circular entre a thread de áudio e quem consome.
//
// Porte de public/wasapi-audio-worklet.js. Os números aqui vieram de medição,
// não de bom senso, e mudá-los quebra o áudio de um jeito difícil de notar:
//
//   O áudio não chega gota a gota, chega em rajadas - a thread do WASAPI acorda
//   com um pacote inteiro. Um teto fixo de 40 ms contra rajadas de 60 ms joga
//   fora a maior parte de cada rajada assim que ela entra. Medido no cliente em
//   Electron: só 66% do áudio era tocado, o resto saía como silêncio.
//
//   Por isso o teto tem piso de 40 ms - mantido baixo para entrega regular
//   continuar com latência baixa - mas cresce até o dobro da maior rajada já
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
        if (amostras == 0 || !intercalado || capacidade_ == 0) return;

        if (amostras > maiorRajada_) {
            maiorRajada_ = amostras;
            teto_.store(std::min(capacidade_, std::max(tetoBase_, amostras * 2)), std::memory_order_relaxed);
        }

        const auto escrita = escrita_.load(std::memory_order_relaxed);
        const auto leitura = leitura_.load(std::memory_order_acquire);
        const size_t cabem = std::min(amostras, capacidade_ - static_cast<size_t>(escrita - leitura));
        for (size_t i = 0; i < cabem; ++i) dados_[(escrita + i) % capacidade_] = intercalado[i];
        escrita_.store(escrita + cabem, std::memory_order_release);
        descartadas_.fetch_add(amostras - cabem, std::memory_order_relaxed);
    }

    // Preenche com o que houver e completa com silêncio. Devolve quantas
    // amostras eram de verdade.
    size_t ler(float* saida, size_t amostras) {
        if (!saida || amostras == 0) return 0;
        auto leitura = leitura_.load(std::memory_order_relaxed);
        const auto escrita = escrita_.load(std::memory_order_acquire);
        size_t disponiveis = static_cast<size_t>(escrita - leitura);
        const auto teto = teto_.load(std::memory_order_relaxed);
        if (disponiveis > teto) {
            const auto excedente = disponiveis - teto;
            leitura += excedente;
            disponiveis = teto;
            descartadas_.fetch_add(excedente, std::memory_order_relaxed);
        }
        const size_t lidas = std::min(amostras, disponiveis);
        for (size_t i = 0; i < lidas; ++i) saida[i] = dados_[(leitura + i) % capacidade_];
        leitura_.store(leitura + lidas, std::memory_order_release);
        std::fill(saida + lidas, saida + amostras, 0.0f);
        return lidas;
    }

    size_t ocupacao() const {
        const auto leitura = leitura_.load(std::memory_order_acquire);
        return static_cast<size_t>(escrita_.load(std::memory_order_acquire) - leitura);
    }
    size_t teto() const { return teto_.load(std::memory_order_relaxed); }
    size_t maiorRajada() const { return maiorRajada_.load(std::memory_order_relaxed); }
    uint64_t descartadas() const { return descartadas_.load(std::memory_order_relaxed); }

private:
    uint32_t canais_;
    size_t capacidade_;
    size_t tetoBase_;
    std::atomic<size_t> teto_;
    std::atomic<size_t> maiorRajada_{0};
    std::atomic<uint64_t> descartadas_{0};

    std::vector<float> dados_;
    std::atomic<uint64_t> escrita_{0};
    std::atomic<uint64_t> leitura_{0};
};

}  // namespace gl
