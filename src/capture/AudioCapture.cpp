#include "capture/AudioCapture.h"

#include <windows.h>

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <atomic>
#include <thread>

#include "util/Log.h"

using Microsoft::WRL::ComPtr;

namespace gl {
namespace {

// ActivateAudioInterfaceAsync exige um handler agile. FtmBase entrega o
// marshaler free-threaded, que é o que torna o objeto agile — no C# isso exigia
// montar a vtable COM na mão porque o CCW do .NET não serve.
class HandlerAtivacao
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::FtmBase,
          IActivateAudioInterfaceCompletionHandler> {
public:
    HandlerAtivacao() {
        pronto_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~HandlerAtivacao() {
        if (pronto_) ::CloseHandle(pronto_);
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operacao) override {
        HRESULT resultadoAtivacao = S_OK;
        ComPtr<IUnknown> desconhecido;

        HRESULT resultado = operacao->GetActivateResult(&resultadoAtivacao, &desconhecido);
        if (SUCCEEDED(resultado)) resultado = resultadoAtivacao;
        if (SUCCEEDED(resultado)) resultado = desconhecido.As(&cliente);

        resultadoFinal = resultado;
        if (pronto_) ::SetEvent(pronto_);
        return S_OK;
    }

    bool esperar(DWORD milissegundos) const {
        return pronto_ && ::WaitForSingleObject(pronto_, milissegundos) == WAIT_OBJECT_0;
    }

    ComPtr<IAudioClient> cliente;
    HRESULT resultadoFinal = E_FAIL;

private:
    HANDLE pronto_ = nullptr;
};

}  // namespace

struct AudioCapture::Interno {
    ComPtr<IAudioClient> cliente;
    ComPtr<IAudioCaptureClient> captura;

    HANDLE eventoAudio = nullptr;
    HANDLE eventoParar = nullptr;
    std::thread thread;

    std::atomic<bool> rodando{false};
    std::atomic<uint64_t> quadrosComFalha{0};

    FormatoAudio formato;
    uint32_t pidExcluido = 0;
    Consumidor consumidor;

    void laco();
};

AudioCapture::AudioCapture() : d_(std::make_unique<Interno>()) {}

AudioCapture::~AudioCapture() { parar(); }

bool AudioCapture::iniciar(uint32_t pidExcluir, Consumidor consumidor) {
    if (d_->rodando.load()) return true;
    if (!consumidor) return false;

    d_->consumidor = std::move(consumidor);
    d_->pidExcluido = pidExcluir;

    AUDIOCLIENT_ACTIVATION_PARAMS parametros{};
    parametros.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    parametros.ProcessLoopbackParams.TargetProcessId = pidExcluir;
    parametros.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT propriedade{};
    propriedade.vt = VT_BLOB;
    propriedade.blob.cbSize = sizeof(parametros);
    propriedade.blob.pBlobData = reinterpret_cast<BYTE*>(&parametros);

    auto handler = Microsoft::WRL::Make<HandlerAtivacao>();
    ComPtr<IActivateAudioInterfaceAsyncOperation> operacao;

    HRESULT resultado = ::ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &propriedade,
        handler.Get(), &operacao);
    if (FAILED(resultado)) {
        erro("ActivateAudioInterfaceAsync falhou: {}", hr(resultado));
        return false;
    }
    if (!handler->esperar(4000)) {
        erro("ativacao do process loopback nao respondeu em 4s");
        return false;
    }
    if (FAILED(handler->resultadoFinal) || !handler->cliente) {
        erro("ativacao do process loopback falhou: {}", hr(handler->resultadoFinal));
        return false;
    }
    d_->cliente = handler->cliente;

    // Process loopback só aceita float32; não adianta perguntar por
    // GetMixFormat, que nem funciona neste dispositivo virtual.
    WAVEFORMATEX formato{};
    formato.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    formato.nChannels = static_cast<WORD>(d_->formato.canais);
    formato.nSamplesPerSec = d_->formato.taxaAmostragem;
    formato.wBitsPerSample = 32;
    formato.nBlockAlign = static_cast<WORD>(formato.nChannels * formato.wBitsPerSample / 8);
    formato.nAvgBytesPerSec = formato.nSamplesPerSec * formato.nBlockAlign;
    formato.cbSize = 0;

    // 200 ms, o mesmo do exemplo oficial ApplicationLoopback da Microsoft. Já
    // esteve em 5 segundos no capturador antigo — tamanho errado para captura
    // em tempo real, e latência de graça.
    constexpr REFERENCE_TIME kDuracaoBuffer = 200 * 10000;

    resultado = d_->cliente->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        kDuracaoBuffer, 0, &formato, nullptr);
    if (FAILED(resultado)) {
        erro("IAudioClient::Initialize falhou: {}", hr(resultado));
        return false;
    }

    d_->eventoAudio = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    d_->eventoParar = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!d_->eventoAudio || !d_->eventoParar) {
        erro("nao foi possivel criar os eventos de audio");
        return false;
    }

    resultado = d_->cliente->SetEventHandle(d_->eventoAudio);
    if (FAILED(resultado)) {
        erro("SetEventHandle falhou: {}", hr(resultado));
        return false;
    }

    resultado = d_->cliente->GetService(IID_PPV_ARGS(&d_->captura));
    if (FAILED(resultado)) {
        erro("GetService(IAudioCaptureClient) falhou: {}", hr(resultado));
        return false;
    }

    resultado = d_->cliente->Start();
    if (FAILED(resultado)) {
        erro("IAudioClient::Start falhou: {}", hr(resultado));
        return false;
    }

    d_->rodando.store(true);
    d_->thread = std::thread([this] { d_->laco(); });

    if (pidExcluir != 0) {
        info("audio: capturando tudo, exceto a arvore do pid {}", pidExcluir);
    } else {
        info("audio: capturando tudo, sem exclusao");
    }
    return true;
}

void AudioCapture::parar() {
    if (!d_->rodando.exchange(false)) return;

    if (d_->eventoParar) ::SetEvent(d_->eventoParar);
    if (d_->thread.joinable()) d_->thread.join();

    if (d_->cliente) d_->cliente->Stop();
    d_->captura.Reset();
    d_->cliente.Reset();

    if (d_->eventoAudio) { ::CloseHandle(d_->eventoAudio); d_->eventoAudio = nullptr; }
    if (d_->eventoParar) { ::CloseHandle(d_->eventoParar); d_->eventoParar = nullptr; }
}

bool AudioCapture::ativo() const { return d_->rodando.load(); }
FormatoAudio AudioCapture::formato() const { return d_->formato; }
uint32_t AudioCapture::pidExcluido() const { return d_->pidExcluido; }
uint64_t AudioCapture::quadrosComFalha() const { return d_->quadrosComFalha.load(); }

void AudioCapture::Interno::laco() {
    // MMCSS: sem isto a thread compete com o resto do sistema e o áudio falha
    // sob carga, que é justamente quando o usuário está jogando e transmitindo.
    DWORD indiceTarefa = 0;
    HANDLE tarefa = ::AvSetMmThreadCharacteristicsW(L"Audio", &indiceTarefa);

    HANDLE esperar[2] = {eventoParar, eventoAudio};

    while (rodando.load()) {
        const DWORD quem = ::WaitForMultipleObjects(2, esperar, FALSE, 200);
        if (quem == WAIT_OBJECT_0) break;  // parar

        // WAIT_TIMEOUT também cai aqui de propósito: em silêncio absoluto o
        // WASAPI pode não sinalizar, e drenar assim mesmo não custa nada.
        UINT32 pacote = 0;
        if (FAILED(captura->GetNextPacketSize(&pacote))) break;

        while (pacote > 0) {
            BYTE* dados = nullptr;
            UINT32 quadros = 0;
            DWORD sinalizadores = 0;

            HRESULT resultado = captura->GetBuffer(&dados, &quadros, &sinalizadores, nullptr, nullptr);
            if (FAILED(resultado)) break;

            if (sinalizadores & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                quadrosComFalha.fetch_add(quadros);
            }

            if (quadros > 0) {
                if (dados && !(sinalizadores & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    // O consumidor recebe o buffer do WASAPI direto, ainda
                    // emprestado: nenhuma cópia acontece neste caminho.
                    consumidor(reinterpret_cast<const float*>(dados), quadros);
                } else {
                    // Silêncio marcado pelo driver: entregar zeros mantém o
                    // relógio do consumidor andando sem inventar dados.
                    thread_local std::vector<float> silencio;
                    const size_t total = static_cast<size_t>(quadros) * formato.canais;
                    if (silencio.size() < total) silencio.assign(total, 0.0f);
                    consumidor(silencio.data(), quadros);
                }
            }

            captura->ReleaseBuffer(quadros);
            if (FAILED(captura->GetNextPacketSize(&pacote))) break;
        }
    }

    if (tarefa) ::AvRevertMmThreadCharacteristics(tarefa);
}

}  // namespace gl
