#pragma once

// Espalha os pacotes RTP no tempo em vez de despejá-los de uma vez.
//
// Um quadro-chave de 1080p passa de 100 KB. Sem nada no meio, o empacotador
// quebra isso em ~85 pacotes e o transporte os entrega ao sistema no mesmo
// instante. Para a placa de rede tudo bem; para o roteador de casa, não: a fila
// de saída dele enche de uma vez, e enquanto ela escoa TODO o resto espera
// atrás - o vídeo que está chegando, o áudio, o ping, o navegador. É por isso
// que "transmitir trava a internet inteira" e o ms dispara.
//
// O navegador não faz isso porque o libwebrtc tem pacer. Este é o nosso: os
// pacotes do quadro saem em fatias de um milissegundo em vez de todos juntos.
//
// Tudo acontece dentro da própria chamada de outgoing(), sem thread e sem fila
// guardada entre chamadas - ver o comentário lá. Custa alguns milissegundos de
// quem chamou, e em troca não há estado que possa sobreviver ao transporte que
// o produziu.
//
// RTCP passa direto: é pequeno, é controle, e atrasar relatório de recepção só
// atrapalharia quem depende dele para reagir.

#include <cstdint>
#include <memory>

#include <rtc/rtc.hpp>

namespace gl {

class Pacer : public rtc::MediaHandler {
public:
    // taxaBits é a taxa alvo do vídeo. O pacer drena um pouco mais rápido que
    // isso (ver kFolga no .cpp): drenar exatamente na taxa do encoder faria a
    // fila crescer para sempre a cada oscilação, e fila é atraso.
    explicit Pacer(uint32_t taxaBits);
    ~Pacer() override;

    Pacer(const Pacer&) = delete;
    Pacer& operator=(const Pacer&) = delete;

    void outgoing(rtc::message_vector& mensagens, const rtc::message_callback& enviar) override;

    // Trocar de qualidade troca a taxa. Vale a partir do próximo pacote.
    void ajustarTaxa(uint32_t taxaBits);

    // Para o relatório: quanto está esperando na fila, em bytes, e quantos
    // pacotes foram descartados por ela ter estourado.
    size_t naFila() const;
    uint64_t descartes() const;

private:
    struct Interno;
    std::unique_ptr<Interno> d_;
};

}  // namespace gl
