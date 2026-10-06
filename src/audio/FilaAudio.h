#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace gl {

// Um produtor (captura) e um consumidor (rede). Somente o consumidor libera
// espacos: sobrescrever um slot em uso misturava bytes de dois pacotes Opus.
class FilaAudio {
public:
    static constexpr size_t kCapacidade = 32;
    static constexpr size_t kMaxPacote = 1500;
    struct Pacote {
        std::array<uint8_t, kMaxPacote> dados{};
        size_t tamanho = 0;
        int64_t tempoUs = 0;
    };

    bool enfileirar(const uint8_t* dados, size_t tamanho, int64_t tempoUs) {
        if (!dados || tamanho == 0 || tamanho > kMaxPacote) return false;
        const auto escrita = escrita_.load(std::memory_order_relaxed);
        if (escrita - leitura_.load(std::memory_order_acquire) >= kCapacidade) return false;
        auto& destino = pacotes_[escrita % kCapacidade];
        std::memcpy(destino.dados.data(), dados, tamanho);
        destino.tamanho = tamanho;
        destino.tempoUs = tempoUs;
        escrita_.store(escrita + 1, std::memory_order_release);
        return true;
    }

    bool retirar(Pacote& destino) {
        const auto escrita = escrita_.load(std::memory_order_acquire);
        auto leitura = leitura_.load(std::memory_order_relaxed);
        if (leitura == escrita) return false;
        // A rede voltou de um atraso: pula o audio antigo sem deixar o
        // produtor reutilizar o slot antes de terminar esta copia.
        if (escrita - leitura > 8) leitura = escrita - 1;
        const auto& origem = pacotes_[leitura % kCapacidade];
        std::memcpy(destino.dados.data(), origem.dados.data(), origem.tamanho);
        destino.tamanho = origem.tamanho;
        destino.tempoUs = origem.tempoUs;
        leitura_.store(leitura + 1, std::memory_order_release);
        return true;
    }

    // Chamado somente com captura e envio parados.
    void limpar() {
        escrita_.store(0, std::memory_order_relaxed);
        leitura_.store(0, std::memory_order_relaxed);
    }

private:
    std::array<Pacote, kCapacidade> pacotes_{};
    alignas(64) std::atomic<uint64_t> escrita_{0};
    alignas(64) std::atomic<uint64_t> leitura_{0};
};
} // namespace gl
