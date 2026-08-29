#pragma once

// Saída de áudio pelo WASAPI: toca o que chega dos outros participantes.

#include <cstdint>
#include <memory>

namespace gl {

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // O que dizer quando o áudio "não está liso": fila curta demais e faltas
    // subindo é rede; velocidade colada no limite é relógio fora de passo;
    // recargas subindo é buraco de verdade.
    struct Estatisticas {
        int filaMs = 0;
        double velocidade = 1.0;
        uint64_t faltas = 0;    // ciclos em que o som acabou no meio
        uint64_t recargas = 0;  // vezes que a fila secou e teve de encher de novo
    };

    bool iniciar();
    void parar();
    bool ativo() const;
    Estatisticas estatisticas() const;

    /// Volume de 0 a 1, aplicado na saida.
    ///
    /// Mexer no volume do Windows abaixaria tudo; isto abaixa so o que chega
    /// da chamada, deixando jogo e musica no volume de sempre.
    void definirVolume(float volume);

    // Enfileira float32 intercalado a 48 kHz. Chamado da thread da rede; a
    // thread de áudio consome por conta própria.
    void enfileirar(const float* intercalado, uint32_t quadros);

private:
    struct Interno;
    std::unique_ptr<Interno> d_;
};

}  // namespace gl
