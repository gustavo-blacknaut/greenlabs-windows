#include "encoder/VideoEncoder.h"

#include <windows.h>

#include <codecapi.h>
#include <icodecapi.h>  // ICodecAPI em si; o codecapi.h so traz os GUIDs
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "util/Log.h"

using Microsoft::WRL::ComPtr;

namespace gl {
namespace {

// Encoders por hardware são assíncronos: em vez de devolver a saída na hora,
// avisam por evento quando querem entrada e quando têm saída. Os de software
// são síncronos. O código lida com os dois, e a diferença fica contida aqui.
bool ehAssincrono(IMFTransform* transformador) {
    ComPtr<IMFAttributes> atributos;
    if (FAILED(transformador->GetAttributes(&atributos))) return false;
    UINT32 valor = 0;
    return SUCCEEDED(atributos->GetUINT32(MF_TRANSFORM_ASYNC, &valor)) && valor != 0;
}

std::string nomeDoAtivador(IMFActivate* ativador) {
    WCHAR* nome = nullptr;
    UINT32 tamanho = 0;
    if (FAILED(ativador->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nome, &tamanho))) {
        return "desconhecido";
    }
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, nome, -1, nullptr, 0, nullptr, nullptr);
    std::string saida(bytes > 1 ? static_cast<size_t>(bytes - 1) : 0, '\0');
    if (bytes > 1) {
        ::WideCharToMultiByte(CP_UTF8, 0, nome, -1, saida.data(), bytes, nullptr, nullptr);
    }
    ::CoTaskMemFree(nome);
    return saida;
}

}  // namespace

struct VideoEncoder::Interno {
    ComPtr<IMFTransform> transformador;
    ComPtr<IMFMediaEventGenerator> eventos;
    ComPtr<IMFDXGIDeviceManager> gerenciador;
    ComPtr<ID3D11Device> dispositivo;

    ConfigEncoder config;
    Consumidor consumidor;

    bool assincrono = false;
    bool hardware = false;
    std::string nome;
    bool iniciado = false;
    UINT resetToken = 0;

    std::atomic<bool> pedidoDeChave{false};
    std::atomic<uint64_t> quadros{0};
    std::atomic<uint64_t> bytes{0};

    // Quando o hardware está assíncrono, um ProcessInput só é aceito depois de
    // um METransformNeedInput. Este contador guarda os pedidos pendentes.
    int entradasPedidas = 0;

    bool configurarTipos();
    bool ajustarCodecApi();
    void drenarEventos();
    std::atomic<uint64_t> quadrosDescartados{0};
    uint64_t eventosNeedInput = 0;
    uint64_t eventosHaveOutput = 0;
    uint64_t eventosOutros = 0;
    HRESULT ultimoErroSaida = S_OK;
    void colherSaida();
};

VideoEncoder::VideoEncoder() : d_(std::make_unique<Interno>()) {}
VideoEncoder::~VideoEncoder() { parar(); }

bool VideoEncoder::iniciar(ID3D11Device* dispositivo, const ConfigEncoder& config,
                           Consumidor consumidor) {
    if (d_->iniciado) return true;
    d_->dispositivo = dispositivo;
    d_->config = config;
    d_->consumidor = std::move(consumidor);

    HRESULT resultado = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(resultado)) {
        erro("MFStartup falhou: {}", hr(resultado));
        return false;
    }

    MFT_REGISTER_TYPE_INFO saidaDesejada{MFMediaType_Video, MFVideoFormat_H264};

    // Duas passadas: primeiro só hardware, depois qualquer um. Sem separar, o
    // MFTEnumEx costuma devolver o de software primeiro e a máquina codifica na
    // CPU sem ninguém perceber.
    for (int passada = 0; passada < 2 && !d_->transformador; ++passada) {
        UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER;
        flags |= (passada == 0) ? MFT_ENUM_FLAG_HARDWARE
                                : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT);

        IMFActivate** ativadores = nullptr;
        UINT32 total = 0;
        if (FAILED(::MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &saidaDesejada,
                               &ativadores, &total)) ||
            total == 0) {
            if (ativadores) ::CoTaskMemFree(ativadores);
            continue;
        }

        for (UINT32 i = 0; i < total; ++i) {
            if (!d_->transformador &&
                SUCCEEDED(ativadores[i]->ActivateObject(IID_PPV_ARGS(&d_->transformador)))) {
                d_->nome = nomeDoAtivador(ativadores[i]);
                d_->hardware = (passada == 0);
            }
            ativadores[i]->Release();
        }
        ::CoTaskMemFree(ativadores);
    }

    if (!d_->transformador) {
        erro("nenhum encoder H.264 disponivel nesta maquina");
        return false;
    }

    ComPtr<IMFAttributes> atributos;
    if (SUCCEEDED(d_->transformador->GetAttributes(&atributos))) {
        // Sem destravar, um MFT assíncrono recusa tudo com E_FAIL e a mensagem
        // não diz o motivo.
        atributos->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        atributos->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    d_->assincrono = ehAssincrono(d_->transformador.Get());

    if (d_->hardware) {
        resultado = ::MFCreateDXGIDeviceManager(&d_->resetToken, &d_->gerenciador);
        if (SUCCEEDED(resultado)) {
            resultado = d_->gerenciador->ResetDevice(d_->dispositivo.Get(), d_->resetToken);
        }
        if (SUCCEEDED(resultado)) {
            // É o que permite ao encoder ler a textura direto da GPU. Sem isto
            // o Media Foundation copiaria cada quadro para a memória principal.
            d_->transformador->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                              reinterpret_cast<ULONG_PTR>(d_->gerenciador.Get()));
        } else {
            aviso("sem gerenciador D3D no encoder ({}), os quadros vao passar pela CPU",
                  hr(resultado));
        }
    }

    if (!d_->configurarTipos()) return false;
    d_->ajustarCodecApi();

    if (d_->assincrono) {
        resultado = d_->transformador.As(&d_->eventos);
        if (FAILED(resultado)) {
            erro("encoder assincrono sem gerador de eventos: {}", hr(resultado));
            return false;
        }
    }

    d_->transformador->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    d_->transformador->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d_->transformador->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    d_->iniciado = true;
    info("encoder H.264: {} ({}, {})", d_->nome, d_->hardware ? "hardware" : "software",
         d_->assincrono ? "assincrono" : "sincrono");
    info("  {}x{} @ {}fps, {} kbps", config.largura, config.altura, config.fps,
         config.bitrate / 1000);
    return true;
}

bool VideoEncoder::Interno::configurarTipos() {
    // A ordem importa: encoders exigem o tipo de saída antes do de entrada,
    // porque é a saída que define o que eles aceitam receber.
    ComPtr<IMFMediaType> tipoSaida;
    HRESULT resultado = ::MFCreateMediaType(&tipoSaida);
    if (FAILED(resultado)) return false;

    tipoSaida->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    tipoSaida->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    tipoSaida->SetUINT32(MF_MT_AVG_BITRATE, config.bitrate);
    tipoSaida->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    tipoSaida->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, FALSE);
    // Baseline não tem quadros B, e quadro B custa latência: ele só pode ser
    // decodificado depois de um quadro futuro chegar. Em tempo real isso é o
    // oposto do que se quer.
    tipoSaida->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_ConstrainedBase);
    ::MFSetAttributeSize(tipoSaida.Get(), MF_MT_FRAME_SIZE, config.largura, config.altura);
    ::MFSetAttributeRatio(tipoSaida.Get(), MF_MT_FRAME_RATE, config.fps, 1);
    ::MFSetAttributeRatio(tipoSaida.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    resultado = transformador->SetOutputType(0, tipoSaida.Get(), 0);
    if (FAILED(resultado)) {
        erro("SetOutputType falhou: {}", hr(resultado));
        return false;
    }

    ComPtr<IMFMediaType> tipoEntrada;
    resultado = ::MFCreateMediaType(&tipoEntrada);
    if (FAILED(resultado)) return false;

    tipoEntrada->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    tipoEntrada->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    tipoEntrada->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    ::MFSetAttributeSize(tipoEntrada.Get(), MF_MT_FRAME_SIZE, config.largura, config.altura);
    ::MFSetAttributeRatio(tipoEntrada.Get(), MF_MT_FRAME_RATE, config.fps, 1);
    ::MFSetAttributeRatio(tipoEntrada.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    resultado = transformador->SetInputType(0, tipoEntrada.Get(), 0);
    if (FAILED(resultado)) {
        erro("SetInputType (NV12 {}x{}) falhou: {}", config.largura, config.altura, hr(resultado));
        return false;
    }
    return true;
}

bool VideoEncoder::Interno::ajustarCodecApi() {
    ComPtr<ICodecAPI> codec;
    if (FAILED(transformador.As(&codec))) return false;

    auto definir = [&](const GUID& propriedade, VARIANT valor) {
        // Nem todo encoder aceita tudo; o que não for suportado é ignorado.
        (void)codec->SetValue(&propriedade, &valor);
    };

    VARIANT v;
    ::VariantInit(&v);

    // Modo de baixa latência: sem lookahead, sem reordenar. É a diferença entre
    // dezenas de milissegundos e alguns.
    v.vt = VT_BOOL;
    v.boolVal = VARIANT_TRUE;
    definir(CODECAPI_AVLowLatencyMode, v);

    v.vt = VT_UI4;
    v.ulVal = eAVEncCommonRateControlMode_CBR;
    definir(CODECAPI_AVEncCommonRateControlMode, v);

    v.vt = VT_UI4;
    v.ulVal = config.bitrate;
    definir(CODECAPI_AVEncCommonMeanBitRate, v);

    // Zero quadros B, pelo mesmo motivo do perfil ConstrainedBaseline.
    v.vt = VT_UI4;
    v.ulVal = 0;
    definir(CODECAPI_AVEncMPVDefaultBPictureCount, v);

    if (config.intervaloChaveSegundos > 0) {
        v.vt = VT_UI4;
        v.ulVal = config.intervaloChaveSegundos * config.fps;
        definir(CODECAPI_AVEncMPVGOPSize, v);
    }
    return true;
}

void VideoEncoder::parar() {
    if (!d_->iniciado) return;
    d_->iniciado = false;

    if (d_->transformador) {
        d_->transformador->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        d_->transformador->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        // Dá um instante para o encoder devolver o que ainda estava dentro dele.
        for (int i = 0; i < 50; ++i) {
            d_->drenarEventos();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        d_->transformador->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }

    info("encoder: {} quadros, {} descartados | eventos: {} need-input, {} have-output, {} outros",
         d_->quadros.load(), d_->quadrosDescartados.load(), d_->eventosNeedInput,
         d_->eventosHaveOutput, d_->eventosOutros);
    if (FAILED(d_->ultimoErroSaida)) {
        aviso("ultimo erro do ProcessOutput: {}", hr(d_->ultimoErroSaida));
    }

    d_->eventos.Reset();
    d_->transformador.Reset();
    d_->gerenciador.Reset();
    ::MFShutdown();
}

void VideoEncoder::pedirQuadroChave() { d_->pedidoDeChave.store(true); }

bool VideoEncoder::codificar(ID3D11Texture2D* nv12, int64_t tempoUs) {
    if (!d_->iniciado || !nv12) return false;

    // Assíncrono: só empurra depois que o encoder pedir. Empurrar antes rende
    // MF_E_NOTACCEPTING e o quadro se perde em silêncio.
    if (d_->assincrono) {
        d_->drenarEventos();
        if (d_->entradasPedidas <= 0) {
            // O encoder ainda está mastigando o quadro anterior. Em tempo real
            // a resposta certa é largar este e seguir para o próximo — esperar
            // só empilharia atraso, que é exatamente o que não se quer.
            d_->quadrosDescartados.fetch_add(1);
            return true;
        }
        d_->entradasPedidas -= 1;
    }

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT resultado =
        ::MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, &buffer);
    if (FAILED(resultado)) {
        erro("MFCreateDXGISurfaceBuffer falhou: {}", hr(resultado));
        return false;
    }

    ComPtr<IMFSample> amostra;
    resultado = ::MFCreateSample(&amostra);
    if (FAILED(resultado)) return false;

    amostra->AddBuffer(buffer.Get());
    // Media Foundation conta em unidades de 100 ns.
    amostra->SetSampleTime(tempoUs * 10);
    amostra->SetSampleDuration(10'000'000LL / d_->config.fps);

    if (d_->pedidoDeChave.exchange(false)) {
        amostra->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        ComPtr<ICodecAPI> codec;
        if (SUCCEEDED(d_->transformador.As(&codec))) {
            VARIANT v;
            ::VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = 1;
            (void)codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
        }
    }

    resultado = d_->transformador->ProcessInput(0, amostra.Get(), 0);
    if (resultado == MF_E_NOTACCEPTING) {
        // Encoder cheio. Colher a saída pendente costuma liberar espaço.
        d_->colherSaida();
        return true;
    }
    if (FAILED(resultado)) {
        erro("ProcessInput falhou: {}", hr(resultado));
        return false;
    }

    if (d_->assincrono) {
        // O MFT publica o HaveOutput e o proximo NeedInput logo depois de
        // aceitar a entrada, mas nao instantaneamente. Uma unica passada quase
        // sempre encontra a fila vazia, e sem o proximo NeedInput o ciclo para:
        // era por isso que a codificacao morria depois de dois quadros.
        // Algumas passadas curtas custam quase nada e mantem o ciclo vivo.
        for (int i = 0; i < 20 && d_->entradasPedidas == 0; ++i) {
            d_->drenarEventos();
            if (d_->entradasPedidas > 0) break;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        d_->drenarEventos();
    } else {
        d_->colherSaida();
    }
    return true;
}

// Consome os eventos que já estão na fila, sem nunca bloquear.
//
// A versão anterior chamava GetEvent sem MF_EVENT_FLAG_NO_WAIT quando queria um
// METransformNeedInput, e isso pendurava o processo inteiro: se o encoder não
// tivesse mais nada a dizer, a espera não terminava nunca. Numa transmissão ao
// vivo não existe motivo para esperar - se o encoder não está pedindo entrada,
// o quadro é descartado e o próximo chega em 16 ms.
void VideoEncoder::Interno::drenarEventos() {
    if (!eventos) {
        colherSaida();
        return;
    }

    for (;;) {
        ComPtr<IMFMediaEvent> evento;
        const HRESULT resultado = eventos->GetEvent(MF_EVENT_FLAG_NO_WAIT, &evento);
        if (FAILED(resultado)) return;  // inclui MF_E_NO_EVENTS_AVAILABLE

        MediaEventType tipo = 0;
        if (FAILED(evento->GetType(&tipo))) return;

        if (tipo == METransformNeedInput) {
            entradasPedidas += 1;
            eventosNeedInput += 1;
        } else if (tipo == METransformHaveOutput) {
            eventosHaveOutput += 1;
            colherSaida();
        } else if (tipo == METransformDrainComplete) {
            return;
        } else {
            eventosOutros += 1;
        }
    }
}

void VideoEncoder::Interno::colherSaida() {
    // Num MFT assincrono vale exatamente um ProcessOutput por METransformHaveOutput.
    // Chamar em laco ate MF_E_TRANSFORM_NEED_MORE_INPUT e o padrao dos sincronos,
    // e num assincrono deixa o transform num estado onde ele para de publicar
    // eventos - foi o que fez a codificacao morrer depois de dois quadros.
    for (int passada = 0;; ++passada) {
        if (assincrono && passada > 0) return;
        MFT_OUTPUT_STREAM_INFO infoSaida{};
        if (FAILED(transformador->GetOutputStreamInfo(0, &infoSaida))) return;

        MFT_OUTPUT_DATA_BUFFER saida{};
        ComPtr<IMFSample> amostra;

        // Quando o MFT não aloca a amostra sozinho, quem chama precisa fornecer.
        const bool alocaSozinho =
            (infoSaida.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                  MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        if (!alocaSozinho) {
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(::MFCreateMemoryBuffer(
                    infoSaida.cbSize ? infoSaida.cbSize : 1 << 20, &buffer))) {
                return;
            }
            if (FAILED(::MFCreateSample(&amostra))) return;
            amostra->AddBuffer(buffer.Get());
            saida.pSample = amostra.Get();
        }

        DWORD estado = 0;
        const HRESULT resultado = transformador->ProcessOutput(0, 1, &saida, &estado);

        if (resultado == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
        if (FAILED(resultado)) ultimoErroSaida = resultado;
        if (resultado == MF_E_TRANSFORM_STREAM_CHANGE) {
            // O encoder renegociou o formato; aceitar e seguir.
            ComPtr<IMFMediaType> novoTipo;
            if (SUCCEEDED(transformador->GetOutputAvailableType(0, 0, &novoTipo))) {
                transformador->SetOutputType(0, novoTipo.Get(), 0);
            }
            continue;
        }
        if (FAILED(resultado)) return;

        ComPtr<IMFSample> obtida = saida.pSample;
        if (saida.pEvents) saida.pEvents->Release();
        if (!obtida) return;

        ComPtr<IMFMediaBuffer> buffer;
        if (SUCCEEDED(obtida->ConvertToContiguousBuffer(&buffer))) {
            BYTE* dados = nullptr;
            DWORD tamanho = 0;
            if (SUCCEEDED(buffer->Lock(&dados, nullptr, &tamanho))) {
                UINT32 pontoLimpo = 0;
                obtida->GetUINT32(MFSampleExtension_CleanPoint, &pontoLimpo);

                LONGLONG tempo = 0;
                obtida->GetSampleTime(&tempo);

                quadros.fetch_add(1);
                bytes.fetch_add(tamanho);

                if (consumidor) {
                    consumidor(PacoteCodificado{dados, tamanho, tempo / 10, pontoLimpo != 0});
                }
                buffer->Unlock();
            }
        }

        // Sem isto o ponteiro cru devolvido pelo ProcessOutput vaza quando o
        // próprio MFT aloca a amostra.
        if (alocaSozinho && saida.pSample) {
            saida.pSample->Release();
            saida.pSample = nullptr;
        }
    }
}

bool VideoEncoder::porHardware() const { return d_->hardware; }
const char* VideoEncoder::nomeDoEncoder() const { return d_->nome.c_str(); }
uint64_t VideoEncoder::quadrosCodificados() const { return d_->quadros.load(); }
uint64_t VideoEncoder::bytesGerados() const { return d_->bytes.load(); }
uint64_t VideoEncoder::quadrosDescartados() const { return d_->quadrosDescartados.load(); }

}  // namespace gl
