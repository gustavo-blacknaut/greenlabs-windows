#include "audio/AudioPlayer.h"

#include <windows.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <cmath>
#include <vector>

#include "audio/AudioCodec.h"
#include "util/Log.h"

using Microsoft::WRL::ComPtr;

namespace gl {
namespace {

// Três medidas, e a diferença entre elas é o que evita estalo.
//
// A fila começa a tocar só depois de juntar kPrefill: começar com ela quase
// vazia faz o primeiro buraco aparecer em seguida, e buraco vira estalo.
//
// O teto é baixo de propósito. Estava em meio segundo, e meio segundo de fila é
// meio segundo de atraso - além de fazer o descarte acontecer o tempo todo, e
// cada descarte é uma emenda audível. Com 200 ms o descarte é raro.
constexpr size_t kAmostrasPorMs = kTaxaAudio * kCanaisAudio / 1000;
constexpr size_t kPrefill = kAmostrasPorMs * 60;
constexpr size_t kMaxAmostras = kAmostrasPorMs * 200;

}  // namespace

struct AudioPlayer::Interno {
    ComPtr<IAudioClient> cliente;
    ComPtr<IAudioRenderClient> render;
    HANDLE evento = nullptr;
    UINT32 buffer = 0;

    std::mutex trava;
    std::vector<float> fila;
    bool enchendo = true;

    // Última amostra tocada e se voltamos de um silêncio: é com isso que as
    // emendas viram rampa em vez de degrau.
    float ultimo = 0.0f;
    bool retomando = true;

    std::atomic<bool> rodando{false};
    std::thread thread;

    void laco();
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

    d_->fila.reserve(kMaxAmostras);
    d_->rodando.store(true);
    d_->cliente->Start();
    d_->thread = std::thread([this] { d_->laco(); });
    info("saida de audio pronta");
    return true;
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

        BYTE* destino = nullptr;
        if (FAILED(render->GetBuffer(livre, &destino))) continue;

        const size_t querem = static_cast<size_t>(livre) * kCanaisAudio;
        size_t copiadas = 0;
        {
            std::lock_guard t(trava);

            // Enchendo: entrega silêncio até juntar a folga inicial. Sem isso a
            // fila sai do zero e seca no primeiro engasgo.
            if (enchendo) {
                if (fila.size() < kPrefill) {
                    render->ReleaseBuffer(livre, AUDCLNT_BUFFERFLAGS_SILENT);
                    continue;
                }
                enchendo = false;
            }

            copiadas = fila.size() < querem ? fila.size() : querem;
            if (copiadas > 0) {
                float* saida = reinterpret_cast<float*>(destino);
                memcpy(saida, fila.data(), copiadas * sizeof(float));
                fila.erase(fila.begin(), fila.begin() + copiadas);

                // Continuidade: começar do zero depois de um silêncio é um
                // degrau na onda, e degrau é estalo. Sobe do último valor
                // tocado ao longo de 5 ms.
                if (retomando) {
                    const size_t rampa = (kAmostrasPorMs * 5 < copiadas) ? kAmostrasPorMs * 5
                                                                         : copiadas;
                    for (size_t i = 0; i < rampa; ++i) {
                        const float f = static_cast<float>(i) / static_cast<float>(rampa);
                        saida[i] = ultimo * (1.0f - f) + saida[i] * f;
                    }
                    retomando = false;
                }

                // O Opus pode devolver amostra além de 1.0, e o WASAPI corta
                // seco no float - corte seco é distorção. Aqui a passagem é
                // suave: só o que passa de 0,95 é comprimido.
                for (size_t i = 0; i < copiadas; ++i) {
                    const float v = saida[i];
                    if (v > 0.95f) {
                        saida[i] = 0.95f + (1.0f - 0.95f) * std::tanh((v - 0.95f) / 0.05f);
                    } else if (v < -0.95f) {
                        saida[i] = -0.95f - (1.0f - 0.95f) * std::tanh((-v - 0.95f) / 0.05f);
                    }
                }
                ultimo = saida[copiadas - 1];
            }
        }

        // Faltou som para encher: o resto vai em silêncio. Melhor um instante
        // mudo que repetir o buffer anterior, que sai como zumbido.
        if (copiadas < querem) {
            // Desce até o silêncio em vez de cortar: um corte no meio da onda
            // é exatamente o estalo que se ouve.
            float* saida = reinterpret_cast<float*>(destino);
            const size_t faltam = querem - copiadas;
            const size_t rampa = (kAmostrasPorMs * 5 < faltam) ? kAmostrasPorMs * 5 : faltam;
            for (size_t i = 0; i < rampa; ++i) {
                const float f = 1.0f - static_cast<float>(i) / static_cast<float>(rampa);
                saida[copiadas + i] = ultimo * f;
            }
            memset(destino + (copiadas + rampa) * sizeof(float), 0,
                   (faltam - rampa) * sizeof(float));
            ultimo = 0.0f;
            retomando = true;

            // Secou: volta a encher antes de tocar de novo. Continuar tocando
            // uma fila vazia produz um estalo a cada ciclo.
            if (copiadas == 0) {
                std::lock_guard t(trava);
                enchendo = true;
            }
        }
        render->ReleaseBuffer(livre, 0);
    }

    if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
}

void AudioPlayer::enfileirar(const float* intercalado, uint32_t quadros) {
    if (!d_->rodando.load() || !intercalado || quadros == 0) return;

    std::lock_guard t(d_->trava);
    const size_t entram = static_cast<size_t>(quadros) * kCanaisAudio;

    // Fila cheia: joga o mais velho fora. Segurar tudo transformaria um engasgo
    // de rede em atraso permanente, que é pior que perder um pedaço.
    if (d_->fila.size() + entram > kMaxAmostras) {
        const size_t sobra = d_->fila.size() + entram - kMaxAmostras;
        const size_t tirar = sobra < d_->fila.size() ? sobra : d_->fila.size();
        d_->fila.erase(d_->fila.begin(), d_->fila.begin() + tirar);
        // A emenda do descarte tambem precisa de rampa.
        d_->retomando = true;
    }
    d_->fila.insert(d_->fila.end(), intercalado, intercalado + entram);
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

}  // namespace gl
