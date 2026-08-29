#pragma once

// Opus, nos dois sentidos.
//
// A captura entrega float32 intercalado a 48 kHz, que é exatamente o que o
// Opus quer nativamente — não há reamostragem no caminho.

#include <cstdint>
#include <memory>
#include <vector>

namespace gl {

// 20 ms é o quadro padrão do WebRTC. Mais curto gasta mais cabeçalho, mais
// longo aumenta a latência sem ganhar quase nada.
inline constexpr uint32_t kTaxaAudio = 48000;
inline constexpr uint32_t kCanaisAudio = 2;
inline constexpr uint32_t kQuadrosPorPacote = kTaxaAudio / 50;  // 960

class AudioEncoder {
public:
    AudioEncoder();
    ~AudioEncoder();

    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    bool iniciar(uint32_t bitrate = 96000);
    void parar();
    bool ativo() const;

    // Recebe o que a captura entregar, no tamanho que vier. Junta internamente
    // até fechar 20 ms e só então devolve um pacote - o WASAPI não entrega em
    // múltiplos certinhos, e mandar pedaço solto quebra o decodificador do
    // outro lado.
    //
    // Junta o que vier. NAO codifica: quem tira os pacotes e o proximo().
    void acumular(const float* intercalado, uint32_t quadros);

    // Um pacote de 20 ms por chamada, ate acabar; vazio quando nao ha mais
    // quadro completo acumulado.
    //
    // Sao dois passos, e nao um, porque o WASAPI nao entrega em fatias de
    // 20 ms - numa volta ele pode trazer 40 ou 60. Com uma funcao so, que
    // recebia e devolvia UM pacote, o resto ficava para tras a cada chamada ate
    // o acumulador estourar e descartar tudo. Assim o chamador drena em laco.
    const std::vector<uint8_t>& proximo();

private:
    struct Interno;
    std::unique_ptr<Interno> d_;
};

class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    bool iniciar();
    void parar();
    bool ativo() const;

    // Devolve float32 intercalado. Vazio quando o pacote não decodifica.
    const std::vector<float>& decodificar(const uint8_t* dados, size_t tamanho);

private:
    struct Interno;
    std::unique_ptr<Interno> d_;
};

}  // namespace gl
