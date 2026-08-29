#include "audio/AudioPlayer.h"

#include <windows.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

#include "audio/AudioCodec.h"
#include "util/Log.h"

using Microsoft::WRL::ComPtr;

namespace gl {
namespace {

// Tudo aqui é contado em quadros (um quadro = uma amostra por canal).
constexpr size_t kQuadrosPorMs = kTaxaAudio / 1000;  // 48

// Quanto som manter guardado. É o atraso que a reprodução acrescenta e, ao
// mesmo tempo, a folga que absorve a oscilação da rede.
constexpr size_t kAlvo = kQuadrosPorMs * 80;

// Fora desta faixa a velocidade começa a ser corrigida.
constexpr size_t kMinimo = kQuadrosPorMs * 40;
constexpr size_t kMaximo = kQuadrosPorMs * 160;

// Onde a correção chega ao desvio máximo.
constexpr size_t kLimite = kQuadrosPorMs * 300;

// Quanto a velocidade pode desviar. 2% é imperceptível — meio tom são 6%.
constexpr double kDesvioMaximo = 0.02;

// Potência de dois para o índice virar máscara em vez de divisão. 682 ms, muito
// mais do que se usa: o que passar disso é rede despejando de uma vez.
constexpr size_t kCapacidade = 32768;
constexpr uint64_t kMascara = kCapacidade - 1;

}  // namespace

struct AudioPlayer::Interno {
    ComPtr<IAudioClient> cliente;
    ComPtr<IAudioRenderClient> render;
    HANDLE evento = nullptr;
    UINT32 buffer = 0;

    // Anel sem trava, um produtor e um consumidor.
    //
    // Antes isto era um vector com mutex, e o mutex era o problema: o erase da
    // frente é um memmove da fila inteira a cada 20 ms, com a trava na mão. E
    // quem enfileira é a thread de rede do libdatachannel — a MESMA que entrega
    // o vídeo. Ou seja: o áudio segurava o vídeo e o vídeo segurava o áudio,
    // que é exatamente o que se ouvia e via quando os dois rodavam juntos.
    //
    // Com o anel, o produtor só escreve e publica um índice; o consumidor só lê
    // e publica o dele. Nenhum dos dois espera pelo outro, e não há memmove.
    float anel[kCapacidade * kCanaisAudio];
    alignas(64) std::atomic<uint64_t> escrita{0};
    alignas(64) std::atomic<uint64_t> leitura{0};

    // Daqui para baixo é estado só do consumidor: ninguém mais toca.
    bool enchendo = true;

    // Leitura em passo fracionário. É isto que permite tocar a 1,02x ou a
    // 0,98x sem cortar nem repetir amostra: o valor entre duas amostras é
    // interpolado, e a onda continua contínua.
    double posicao = 0.0;
    double velocidade = 1.0;

    // Última amostra de cada canal, para as emendas virarem rampa, não degrau.
    float ultimo[kCanaisAudio] = {0.0f, 0.0f};
    bool retomando = true;

    std::atomic<bool> rodando{false};
    std::thread thread;

    /// Ganho aplicado na saida. Lido pela thread de audio a cada ciclo.
    std::atomic<float> volume{1.0f};

    // Só para o relatório: escritas relaxed, ninguém decide nada com elas.
    std::atomic<uint64_t> faltas{0};
    std::atomic<uint64_t> recargas{0};
    std::atomic<int> filaMs{0};
    std::atomic<double> velocidadeVista{1.0};

    void laco();
    void ajustarVelocidade(size_t disponivel);
};

AudioPlayer::AudioPlayer() : d_(std::make_unique<Interno>()) {}
AudioPlayer::~AudioPlayer() { parar(); }

bool AudioPlayer::iniciar() {
    if (d_->rodando.load()) return true;

    ComPtr<IMMDeviceEnumerator> enumerador;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerador)))) {
        return false;
    }

    ComPtr<IMMDevice> dispositivo;
    if (FAILED(enumerador->GetDefaultAudioEndpoint(eRender, eConsole, &dispositivo))) return false;
    if (FAILED(dispositivo->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(d_->cliente.GetAddressOf())))) {
        return false;
    }

    // Float32 a 48 kHz estéreo: o mesmo formato que sai do decodificador, então
    // não há conversão no caminho.
    WAVEFORMATEXTENSIBLE formato{};
    formato.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    formato.Format.nChannels = static_cast<WORD>(kCanaisAudio);
    formato.Format.nSamplesPerSec = kTaxaAudio;
    formato.Format.wBitsPerSample = 32;
    formato.Format.nBlockAlign = static_cast<WORD>(kCanaisAudio * 4);
    formato.Format.nAvgBytesPerSec = kTaxaAudio * formato.Format.nBlockAlign;
    formato.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    formato.Samples.wValidBitsPerSample = 32;
    formato.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    formato.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    const REFERENCE_TIME duracao = 20 * 10000;  // 20 ms
    if (FAILED(d_->cliente->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                       AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                           AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                       duracao, 0,
                                       reinterpret_cast<WAVEFORMATEX*>(&formato), nullptr))) {
        erro("saida de audio: Initialize falhou");
        return false;
    }

    d_->evento = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!d_->evento || FAILED(d_->cliente->SetEventHandle(d_->evento))) return false;
    if (FAILED(d_->cliente->GetBufferSize(&d_->buffer))) return false;
    if (FAILED(d_->cliente->GetService(IID_PPV_ARGS(&d_->render)))) return false;

    d_->rodando.store(true);
    d_->cliente->Start();
    d_->thread = std::thread([this] { d_->laco(); });
    info("saida de audio: alvo 80 ms, anel sem trava, correcao de ate 2%");
    return true;
}

// ajustarVelocidade decide o passo de leitura a partir de quanto som há guardado.
//
// A fila oscila por dois motivos: a rede entrega irregular, e o relógio de quem
// manda nunca é exatamente igual ao de quem toca — alguns quadros por hora de
// diferença já enchem ou esvaziam a fila.
//
// A resposta antiga era descartar (emenda audível) ou inserir silêncio (buraco
// audível). A resposta certa é consumir um pouco mais rápido ou mais devagar
// até voltar ao alvo. A 2% ninguém ouve, e a própria mudança de velocidade é
// gradual para também não aparecer.
void AudioPlayer::Interno::ajustarVelocidade(size_t disponivel) {
    double desejada = 1.0;

    if (disponivel > kMaximo) {
        const double excesso =
            static_cast<double>(disponivel - kMaximo) / static_cast<double>(kLimite - kMaximo);
        desejada = 1.0 + kDesvioMaximo * (excesso < 1.0 ? excesso : 1.0);
    } else if (disponivel < kMinimo) {
        const double falta =
            static_cast<double>(kMinimo - disponivel) / static_cast<double>(kMinimo);
        desejada = 1.0 - kDesvioMaximo * (falta < 1.0 ? falta : 1.0);
    }

    velocidade += (desejada - velocidade) * 0.05;
}

void AudioPlayer::Interno::laco() {
    // Mesma prioridade da captura: a thread de áudio não pode ser preterida,
    // senão o buffer seca e sai estalo.
    DWORD tarefa = 0;
    HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &tarefa);

    while (rodando.load()) {
        if (::WaitForSingleObject(evento, 200) != WAIT_OBJECT_0) continue;

        UINT32 emUso = 0;
        if (FAILED(cliente->GetCurrentPadding(&emUso))) continue;
        const UINT32 livre = buffer - emUso;
        if (livre == 0) continue;

        const uint64_t r = leitura.load(std::memory_order_relaxed);
        const size_t disponivel = static_cast<size_t>(escrita.load(std::memory_order_acquire) - r);

        filaMs.store(static_cast<int>(disponivel / kQuadrosPorMs), std::memory_order_relaxed);
        velocidadeVista.store(velocidade, std::memory_order_relaxed);

        // Enchendo: silêncio até juntar a folga. Tocar uma fila quase vazia faz
        // o buraco seguinte aparecer logo, e buraco vira estalo.
        if (enchendo) {
            if (disponivel < kAlvo) {
                BYTE* mudo = nullptr;
                if (SUCCEEDED(render->GetBuffer(livre, &mudo))) {
                    render->ReleaseBuffer(livre, AUDCLNT_BUFFERFLAGS_SILENT);
                }
                continue;
            }
            enchendo = false;
            posicao = 0.0;
            velocidade = 1.0;
        }

        BYTE* bruto = nullptr;
        if (FAILED(render->GetBuffer(livre, &bruto))) continue;
        float* saida = reinterpret_cast<float*>(bruto);

        ajustarVelocidade(disponivel);

        size_t entregues = 0;
        for (UINT32 q = 0; q < livre; ++q) {
            const size_t inteiro = static_cast<size_t>(posicao);
            if (inteiro + 1 >= disponivel) break;

            // Interpolação linear entre as duas amostras vizinhas: é o que
            // torna o passo fracionário possível sem produzir degrau.
            const float fracao = static_cast<float>(posicao - static_cast<double>(inteiro));
            const size_t a = static_cast<size_t>((r + inteiro) & kMascara) * kCanaisAudio;
            const size_t b = static_cast<size_t>((r + inteiro + 1) & kMascara) * kCanaisAudio;
            for (size_t c = 0; c < kCanaisAudio; ++c) {
                saida[q * kCanaisAudio + c] = anel[a + c] + (anel[b + c] - anel[a + c]) * fracao;
            }
            posicao += velocidade;
            ++entregues;
        }

        // Publica o que passou e guarda a parte fracionária: sem isso o passo
        // perderia a fase a cada ciclo.
        const size_t consumidos = static_cast<size_t>(posicao);
        if (consumidos > 0) {
            leitura.store(r + consumidos, std::memory_order_release);
            posicao -= static_cast<double>(consumidos);
        }

        if (entregues > 0) {
            const size_t total = entregues * kCanaisAudio;

            // Sair do silêncio direto para a onda é degrau, e degrau é estalo.
            if (retomando) {
                const size_t rampa =
                    (kQuadrosPorMs * 5 * kCanaisAudio < total) ? kQuadrosPorMs * 5 * kCanaisAudio
                                                               : total;
                for (size_t i = 0; i < rampa; ++i) {
                    const float f = static_cast<float>(i) / static_cast<float>(rampa);
                    saida[i] = ultimo[i % kCanaisAudio] * (1.0f - f) + saida[i] * f;
                }
                retomando = false;
            }

            // O volume entra ANTES do limitador: baixando o som, nada mais
            // encosta no teto, e o limitador para de agir - que e o certo.
            // Aplicado depois, ele comprimiria com base no sinal cheio e
            // deixaria o som abafado mesmo baixo.
            const float ganho = volume.load(std::memory_order_relaxed);
            if (ganho != 1.0f) {
                for (size_t i = 0; i < total; ++i) saida[i] *= ganho;
            }

            // O Opus devolve amostra além de 1.0 e o WASAPI corta seco, o que é
            // distorção. Aqui só o que passa de 0,95 é comprimido.
            for (size_t i = 0; i < total; ++i) {
                const float v = saida[i];
                if (v > 0.95f) {
                    saida[i] = 0.95f + 0.05f * std::tanh((v - 0.95f) / 0.05f);
                } else if (v < -0.95f) {
                    saida[i] = -0.95f - 0.05f * std::tanh((-v - 0.95f) / 0.05f);
                }
            }
            for (size_t c = 0; c < kCanaisAudio; ++c) {
                ultimo[c] = saida[total - kCanaisAudio + c];
            }
        }

        // Faltou som: desce até o silêncio em vez de cortar no meio da onda.
        if (entregues < livre) {
            const size_t inicio = entregues * kCanaisAudio;
            const size_t faltam = (livre - entregues) * kCanaisAudio;
            const size_t rampa =
                (kQuadrosPorMs * 5 * kCanaisAudio < faltam) ? kQuadrosPorMs * 5 * kCanaisAudio
                                                            : faltam;
            for (size_t i = 0; i < rampa; ++i) {
                const float f = 1.0f - static_cast<float>(i) / static_cast<float>(rampa);
                saida[inicio + i] = ultimo[i % kCanaisAudio] * f;
            }
            memset(saida + inicio + rampa, 0, (faltam - rampa) * sizeof(float));
            ultimo[0] = ultimo[1] = 0.0f;
            retomando = true;
            enchendo = true;
            faltas.fetch_add(1, std::memory_order_relaxed);
            recargas.fetch_add(1, std::memory_order_relaxed);
        }

        render->ReleaseBuffer(livre, 0);
    }

    if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
}

// Chamada da thread de rede. Só escreve e publica: nada de trava, nada de
// alocação, nada que possa fazer essa thread esperar - ela também é quem
// entrega o vídeo.
void AudioPlayer::enfileirar(const float* intercalado, uint32_t quadros) {
    if (!d_->rodando.load() || !intercalado || quadros == 0) return;

    const uint64_t w = d_->escrita.load(std::memory_order_relaxed);
    const uint64_t r = d_->leitura.load(std::memory_order_acquire);

    const size_t livres = kCapacidade - static_cast<size_t>(w - r);
    const size_t cabem = quadros < livres ? quadros : livres;
    if (cabem == 0) return;

    const size_t inicio = static_cast<size_t>(w & kMascara);
    const size_t ateOFim = kCapacidade - inicio;
    const size_t primeira = cabem < ateOFim ? cabem : ateOFim;

    memcpy(d_->anel + inicio * kCanaisAudio, intercalado,
           primeira * kCanaisAudio * sizeof(float));
    if (cabem > primeira) {
        memcpy(d_->anel, intercalado + primeira * kCanaisAudio,
               (cabem - primeira) * kCanaisAudio * sizeof(float));
    }

    d_->escrita.store(w + cabem, std::memory_order_release);
}

void AudioPlayer::parar() {
    if (!d_->rodando.exchange(false)) return;
    if (d_->thread.joinable()) d_->thread.join();
    if (d_->cliente) d_->cliente->Stop();
    if (d_->evento) ::CloseHandle(d_->evento);
    d_->evento = nullptr;
    d_->render.Reset();
    d_->cliente.Reset();
}

bool AudioPlayer::ativo() const { return d_->rodando.load(); }

void AudioPlayer::definirVolume(float volume) {
    d_->volume.store(volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume),
                    std::memory_order_relaxed);
}

AudioPlayer::Estatisticas AudioPlayer::estatisticas() const {
    Estatisticas e;
    e.filaMs = d_->filaMs.load(std::memory_order_relaxed);
    e.velocidade = d_->velocidadeVista.load(std::memory_order_relaxed);
    e.faltas = d_->faltas.load(std::memory_order_relaxed);
    e.recargas = d_->recargas.load(std::memory_order_relaxed);
    return e;
}

}  // namespace gl
