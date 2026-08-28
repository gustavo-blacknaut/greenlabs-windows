#include "audio/AudioCodec.h"

#include <opus.h>

#include <cstring>

#include "util/Log.h"

namespace gl {

struct AudioEncoder::Interno {
    OpusEncoder* enc = nullptr;

    // O WASAPI entrega o que tem, não múltiplos de 20 ms. Aqui se junta até
    // fechar um quadro; o que sobra fica para a próxima chamada.
    //
    // Buffer fixo, e não vector: isto roda na thread de tempo real do áudio, e
    // tanto o insert (que pode realocar) quanto o erase (que faz memmove do
    // resto) custam caro num lugar que não pode custar nada. Cabem dois quadros
    // com folga, que é mais do que o WASAPI entrega de uma vez.
    static constexpr size_t kCapacidade = kQuadrosPorPacote * kCanaisAudio * 4;
    float acumulado[kCapacidade];
    size_t usado = 0;
    std::vector<uint8_t> saida;
};

AudioEncoder::AudioEncoder() : d_(std::make_unique<Interno>()) {}
AudioEncoder::~AudioEncoder() { parar(); }

bool AudioEncoder::iniciar(uint32_t bitrate) {
    if (d_->enc) return true;

    int erroOpus = 0;
    d_->enc = ::opus_encoder_create(kTaxaAudio, kCanaisAudio, OPUS_APPLICATION_AUDIO, &erroOpus);
    if (!d_->enc || erroOpus != OPUS_OK) {
        erro("opus_encoder_create falhou: {}", erroOpus);
        d_->enc = nullptr;
        return false;
    }

    ::opus_encoder_ctl(d_->enc, OPUS_SET_BITRATE(static_cast<int32_t>(bitrate)));
    // Tempo real: sem isto o codificador atrasa para decidir melhor, e atraso
    // em chamada é pior que um bit a mais.
    ::opus_encoder_ctl(d_->enc, OPUS_SET_COMPLEXITY(5));
    ::opus_encoder_ctl(d_->enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

    d_->usado = 0;
    d_->saida.resize(4000);
    info("audio: Opus {} kbps, {} Hz, {} canais", bitrate / 1000, kTaxaAudio, kCanaisAudio);
    return true;
}

void AudioEncoder::parar() {
    if (!d_->enc) return;
    ::opus_encoder_destroy(d_->enc);
    d_->enc = nullptr;
    d_->usado = 0;
}

bool AudioEncoder::ativo() const { return d_->enc != nullptr; }

const std::vector<uint8_t>& AudioEncoder::codificar(const float* intercalado, uint32_t quadros) {
    static const std::vector<uint8_t> vazio;
    if (!d_->enc || !intercalado || quadros == 0) return vazio;

    const size_t entram = static_cast<size_t>(quadros) * kCanaisAudio;

    // Não cabe: o consumidor está atrasado. Descartar o que já havia é melhor
    // que crescer o buffer - som velho não interessa e atraso não se recupera.
    if (d_->usado + entram > Interno::kCapacidade) d_->usado = 0;
    if (entram > Interno::kCapacidade) return vazio;

    memcpy(d_->acumulado + d_->usado, intercalado, entram * sizeof(float));
    d_->usado += entram;

    const size_t precisa = static_cast<size_t>(kQuadrosPorPacote) * kCanaisAudio;
    if (d_->usado < precisa) return vazio;

    // O buffer de saída volta ao tamanho de trabalho antes de cada chamada: o
    // resize do fim da função encolhe, e opus_encode precisa do espaço todo.
    if (d_->saida.size() < 4000) d_->saida.resize(4000);

    const int bytes = ::opus_encode_float(d_->enc, d_->acumulado, kQuadrosPorPacote,
                                          d_->saida.data(),
                                          static_cast<opus_int32>(d_->saida.size()));

    d_->usado -= precisa;
    if (d_->usado > 0) memmove(d_->acumulado, d_->acumulado + precisa, d_->usado * sizeof(float));

    if (bytes <= 0) return vazio;

    // Sem cópia: devolve o próprio buffer, só ajustando o tamanho. A capacidade
    // já está alocada, então resize aqui não vai ao alocador - e isto roda na
    // thread de tempo real do áudio, onde alocar sai como estalo.
    d_->saida.resize(static_cast<size_t>(bytes));
    return d_->saida;
}

struct AudioDecoder::Interno {
    OpusDecoder* dec = nullptr;
    std::vector<float> saida;
    size_t quadrosUltimo = 0;
};

AudioDecoder::AudioDecoder() : d_(std::make_unique<Interno>()) {}
AudioDecoder::~AudioDecoder() { parar(); }

bool AudioDecoder::iniciar() {
    if (d_->dec) return true;
    int erroOpus = 0;
    d_->dec = ::opus_decoder_create(kTaxaAudio, kCanaisAudio, &erroOpus);
    if (!d_->dec || erroOpus != OPUS_OK) {
        erro("opus_decoder_create falhou: {}", erroOpus);
        d_->dec = nullptr;
        return false;
    }
    // Espaço para o maior quadro que o Opus admite (120 ms).
    d_->saida.resize(static_cast<size_t>(kTaxaAudio / 1000 * 120) * kCanaisAudio);
    return true;
}

void AudioDecoder::parar() {
    if (!d_->dec) return;
    ::opus_decoder_destroy(d_->dec);
    d_->dec = nullptr;
}

bool AudioDecoder::ativo() const { return d_->dec != nullptr; }

const std::vector<float>& AudioDecoder::decodificar(const uint8_t* dados, size_t tamanho) {
    static const std::vector<float> vazio;
    if (!d_->dec || !dados || tamanho == 0) return vazio;

    // Devolve ao tamanho cheio: o resize do fim da chamada anterior encolheu.
    const size_t cheio = static_cast<size_t>(kTaxaAudio / 1000 * 120) * kCanaisAudio;
    if (d_->saida.size() < cheio) d_->saida.resize(cheio);

    const int quadros = ::opus_decode_float(
        d_->dec, dados, static_cast<opus_int32>(tamanho), d_->saida.data(),
        static_cast<int>(d_->saida.size() / kCanaisAudio), 0);
    if (quadros <= 0) return vazio;

    d_->quadrosUltimo = static_cast<size_t>(quadros) * kCanaisAudio;
    d_->saida.resize(d_->quadrosUltimo);
    return d_->saida;
}

}  // namespace gl
