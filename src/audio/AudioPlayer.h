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

    bool iniciar();
    void parar();
    bool ativo() const;

    // Enfileira float32 intercalado a 48 kHz. Chamado da thread da rede; a
    // thread de áudio consome por conta própria.
    void enfileirar(const float* intercalado, uint32_t quadros);

private:
    struct Interno;
    std::unique_ptr<Interno> d_;
};

}  // namespace gl
