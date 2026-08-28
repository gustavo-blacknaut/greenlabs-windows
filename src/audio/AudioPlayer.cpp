#include "audio/AudioPlayer.h"

#include <windows.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <thread>
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
                memcpy(destino, fila.data(), copiadas * sizeof(float));
                fila.erase(fila.begin(), fila.begin() + copiadas);
            }
        }

        // Faltou som para encher: o resto vai em silêncio. Melhor um instante
        // mudo que repetir o buffer anterior, que sai como zumbido.
        if (copiadas < querem) {
            memset(destino + copiadas * sizeof(float), 0, (querem - copiadas) * sizeof(float));

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
