#include "capture/ScreenCapture.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <chrono>
#include <cstring>

#include "util/Log.h"

using Microsoft::WRL::ComPtr;

namespace gl {
namespace {

// Meio segundo entre tentativas de reabrir a duplicação. Curto o bastante para
// a captura voltar sozinha assim que o UAC ou o jogo em tela cheia saírem da
// frente, e longo o bastante para uma recusa permanente não virar um laço.
constexpr int kIntervaloEntreTentativasMs = 500;

std::string paraUtf8(const wchar_t* bruto) {
    if (!bruto || !*bruto) return {};
    const int tamanho = ::WideCharToMultiByte(CP_UTF8, 0, bruto, -1, nullptr, 0, nullptr, nullptr);
    if (tamanho <= 1) return {};
    std::string saida(static_cast<size_t>(tamanho - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, bruto, -1, saida.data(), tamanho, nullptr, nullptr);
    return saida;
}

int64_t frequenciaQpc() {
    static const int64_t f = [] {
        LARGE_INTEGER v{};
        ::QueryPerformanceFrequency(&v);
        return v.QuadPart;
    }();
    return f;
}

int64_t agoraQpc() {
    LARGE_INTEGER v{};
    ::QueryPerformanceCounter(&v);
    return v.QuadPart;
}

// A rotação que o DXGI informa, em graus.
//
// DXGI_MODE_ROTATION_UNSPECIFIED aparece em monitor único e significa "sem
// giro"; tratá-lo como desconhecido faria a tela girar sem motivo.
uint32_t grausDe(DXGI_MODE_ROTATION rotacao) {
    switch (rotacao) {
        case DXGI_MODE_ROTATION_ROTATE90:  return 90;
        case DXGI_MODE_ROTATION_ROTATE180: return 180;
        case DXGI_MODE_ROTATION_ROTATE270: return 270;
        default:                           return 0;
    }
}

// Percorre adaptadores e saídas chamando a função para cada par encontrado.
template <class Fn>
void paraCadaSaida(Fn&& fn) {
    ComPtr<IDXGIFactory1> fabrica;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&fabrica)))) return;

    for (UINT ia = 0;; ++ia) {
        ComPtr<IDXGIAdapter1> adaptador;
        if (fabrica->EnumAdapters1(ia, &adaptador) == DXGI_ERROR_NOT_FOUND) break;

        for (UINT is = 0;; ++is) {
            ComPtr<IDXGIOutput> saida;
            if (adaptador->EnumOutputs(is, &saida) == DXGI_ERROR_NOT_FOUND) break;
            if (!fn(adaptador, saida)) return;
        }
    }
}

}  // namespace

struct ScreenCapture::Interno {
    ComPtr<ID3D11Device> dispositivo;
    ComPtr<ID3D11DeviceContext> contexto;
    ComPtr<IDXGIOutputDuplication> duplicacao;
    ComPtr<IDXGIAdapter1> adaptador;
    ComPtr<IDXGIOutput1> saida;
    ComPtr<ID3D11Texture2D> texturaAtual;

    MonitorInfo info;
    bool quadroEmMaos = false;

    // Posição e visibilidade do ponteiro persistem entre quadros: o DXGI só
    // avisa quando mudam. Guardar aqui evita o cursor sumir a cada quadro em
    // que o mouse ficou parado.
    bool cursorVisivel = false;
    int32_t cursorX = 0;
    int32_t cursorY = 0;
    FormaCursor forma;
    std::vector<uint8_t> bufferForma;

    // Quando a duplicacao esta negada de verdade, o laco pede reinicio a cada
    // quadro. Sem intervalo minimo isso vira dezenas de tentativas por segundo.
    std::chrono::steady_clock::time_point ultimaTentativa{};

    void lerFormaDoCursor(const DXGI_OUTDUPL_FRAME_INFO& informacao);

    bool criarDuplicacao();
};

ScreenCapture::ScreenCapture() : d_(std::make_unique<Interno>()) {}
ScreenCapture::~ScreenCapture() { parar(); }

std::vector<MonitorInfo> ScreenCapture::listarMonitores() {
    std::vector<MonitorInfo> monitores;
    uint32_t indice = 0;

    paraCadaSaida([&](ComPtr<IDXGIAdapter1>&, ComPtr<IDXGIOutput>& saida) {
        DXGI_OUTPUT_DESC descricao{};
        if (SUCCEEDED(saida->GetDesc(&descricao)) && descricao.AttachedToDesktop) {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            const bool primario = ::GetMonitorInfoW(descricao.Monitor, &mi) &&
                                  (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

            monitores.push_back(MonitorInfo{
                indice,
                paraUtf8(descricao.DeviceName),
                static_cast<uint32_t>(descricao.DesktopCoordinates.right -
                                      descricao.DesktopCoordinates.left),
                static_cast<uint32_t>(descricao.DesktopCoordinates.bottom -
                                      descricao.DesktopCoordinates.top),
                primario,
                grausDe(descricao.Rotation),
            });
            ++indice;
        }
        return true;
    });
    return monitores;
}

bool ScreenCapture::iniciar(uint32_t indiceMonitor) {
    parar();

    uint32_t visto = 0;
    paraCadaSaida([&](ComPtr<IDXGIAdapter1>& adaptador, ComPtr<IDXGIOutput>& saida) {
        DXGI_OUTPUT_DESC descricao{};
        if (FAILED(saida->GetDesc(&descricao)) || !descricao.AttachedToDesktop) return true;
        if (visto++ != indiceMonitor) return true;

        d_->adaptador = adaptador;
        saida.As(&d_->saida);
        d_->info.indice = indiceMonitor;
        d_->info.nome = paraUtf8(descricao.DeviceName);
        d_->info.largura = static_cast<uint32_t>(descricao.DesktopCoordinates.right -
                                                 descricao.DesktopCoordinates.left);
        d_->info.altura = static_cast<uint32_t>(descricao.DesktopCoordinates.bottom -
                                               descricao.DesktopCoordinates.top);
        d_->info.graus = grausDe(descricao.Rotation);
        return false;  // achou, para de procurar
    });

    if (!d_->saida) {
        erro("monitor {} nao encontrado", indiceMonitor);
        return false;
    }

    // O dispositivo D3D11 precisa ser do MESMO adaptador da saída, senão o
    // DuplicateOutput devolve DXGI_ERROR_UNSUPPORTED. É o caso comum em
    // notebook com placa híbrida.
    const D3D_FEATURE_LEVEL niveis[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
    };
    D3D_FEATURE_LEVEL nivelObtido{};

    // Escada de tentativas, em vez de uma só.
    //
    // Pedir VIDEO_SUPPORT de saída é o certo - é o que dá encoder e
    // decodificador na GPU. Mas há máquina e há momento em que o driver
    // simplesmente recusa (DXGI_ERROR_UNSUPPORTED), e antes disso derrubava o
    // aplicativo inteiro na abertura: caixa de erro, nada funciona, fim.
    //
    // Sem aceleração de vídeo o programa continua útil - a tela é capturada, o
    // codec cai para software, a chamada acontece. E se nem o adaptador servir,
    // o WARP serve: é o rasterizador por software da própria Microsoft, mais
    // lento e sempre disponível. É a mesma escada que o Chromium desce, e é por
    // isso que o Electron abre em máquina onde isto não abria.
    struct Tentativa {
        IDXGIAdapter1* adaptador;
        D3D_DRIVER_TYPE tipo;
        UINT sinalizadores;
        const char* descricao;
    };
    const UINT comVideo = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const UINT semVideo = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    const Tentativa tentativas[] = {
        {d_->adaptador.Get(), D3D_DRIVER_TYPE_UNKNOWN, comVideo, "GPU com aceleracao de video"},
        {d_->adaptador.Get(), D3D_DRIVER_TYPE_UNKNOWN, semVideo, "GPU sem aceleracao de video"},
        {nullptr, D3D_DRIVER_TYPE_HARDWARE, comVideo, "GPU padrao com aceleracao de video"},
        {nullptr, D3D_DRIVER_TYPE_HARDWARE, semVideo, "GPU padrao sem aceleracao de video"},
        {nullptr, D3D_DRIVER_TYPE_WARP, semVideo, "WARP (software)"},
    };

    HRESULT resultado = E_FAIL;
    const char* escolhida = nullptr;
    for (const Tentativa& t : tentativas) {
        d_->dispositivo.Reset();
        d_->contexto.Reset();
        resultado = ::D3D11CreateDevice(t.adaptador, t.tipo, nullptr, t.sinalizadores, niveis,
                                        ARRAYSIZE(niveis), D3D11_SDK_VERSION, &d_->dispositivo,
                                        &nivelObtido, &d_->contexto);
        if (SUCCEEDED(resultado)) {
            escolhida = t.descricao;
            break;
        }
        aviso("D3D11: {} nao serviu ({})", t.descricao, hr(resultado));
    }

    if (!escolhida) {
        erro("D3D11CreateDevice falhou em todas as tentativas: {}", hr(resultado));
        return false;
    }

    // Vale registrar qual ganhou: a diferença entre a primeira e a última é a
    // diferença entre transmitir 1080p sem sentir e a máquina inteira sofrer.
    info("D3D11: {}", escolhida);
    if (escolhida != tentativas[0].descricao) {
        aviso("sem o caminho preferido de video - o codec pode cair para software");
    }

    // Duplicacao que nao abre agora nao impede o aplicativo de rodar: da para
    // entrar na sala, assistir e usar a camera sem ela. proximoQuadro() devolve
    // PrecisaReiniciar enquanto faltar, e o laco tenta de novo.
    if (!d_->criarDuplicacao()) {
        aviso("captura de tela indisponivel por enquanto; o resto do aplicativo funciona");
    }

    info("tela: monitor {} ({}) {}x{}", d_->info.indice, d_->info.nome, d_->info.largura,
         d_->info.altura);
    return true;
}

bool ScreenCapture::Interno::criarDuplicacao() {
    duplicacao.Reset();
    const HRESULT resultado = saida->DuplicateOutput(dispositivo.Get(), &duplicacao);
    if (FAILED(resultado)) {
        if (resultado == E_ACCESSDENIED) {
            // Acontece durante o UAC, na tela de bloqueio e com jogo em tela
            // cheia exclusiva. Não é fatal: tenta de novo depois.
            aviso("duplicacao negada agora (tela protegida ou jogo em tela cheia exclusiva)");
        } else if (resultado == DXGI_ERROR_UNSUPPORTED) {
            erro("duplicacao nao suportada neste adaptador: {}", hr(resultado));
        } else {
            erro("DuplicateOutput falhou: {}", hr(resultado));
        }
        return false;
    }
    return true;
}

bool ScreenCapture::capturando() const { return d_->duplicacao != nullptr; }

bool ScreenCapture::reiniciar() {
    using namespace std::chrono;

    const auto agora = steady_clock::now();
    if (d_->duplicacao == nullptr &&
        agora - d_->ultimaTentativa < milliseconds(kIntervaloEntreTentativasMs)) {
        return false;
    }
    d_->ultimaTentativa = agora;

    liberarQuadro();
    return d_->criarDuplicacao();
}

void ScreenCapture::parar() {
    liberarQuadro();
    d_->duplicacao.Reset();
    d_->saida.Reset();
    d_->adaptador.Reset();
    d_->contexto.Reset();
    d_->dispositivo.Reset();
}

ResultadoQuadro ScreenCapture::proximoQuadro(uint32_t prazoMs, QuadroCapturado& saida) {
    if (!d_->duplicacao) return ResultadoQuadro::PrecisaReiniciar;
    if (d_->quadroEmMaos) {
        // O DXGI exige ReleaseFrame antes do próximo Acquire. Chamar duas vezes
        // seguidas sem liberar devolve erro e trava a captura.
        liberarQuadro();
    }

    DXGI_OUTDUPL_FRAME_INFO informacao{};
    ComPtr<IDXGIResource> recurso;

    const HRESULT resultado = d_->duplicacao->AcquireNextFrame(prazoMs, &informacao, &recurso);
    if (resultado == DXGI_ERROR_WAIT_TIMEOUT) {
        return ResultadoQuadro::SemMudanca;
    }
    if (resultado == DXGI_ERROR_ACCESS_LOST) {
        return ResultadoQuadro::PrecisaReiniciar;
    }
    if (FAILED(resultado)) {
        erro("AcquireNextFrame falhou: {}", hr(resultado));
        return ResultadoQuadro::Erro;
    }

    d_->quadroEmMaos = true;

    // O DXGI só reporta o ponteiro quando ele muda. LastMouseUpdateTime em zero
    // significa "nada mudou", e ler PointerPosition nesse caso traria lixo.
    if (informacao.LastMouseUpdateTime.QuadPart != 0) {
        d_->cursorVisivel = informacao.PointerPosition.Visible != FALSE;
        d_->cursorX = informacao.PointerPosition.Position.x;
        d_->cursorY = informacao.PointerPosition.Position.y;
    }
    saida.formaMudou = informacao.PointerShapeBufferSize > 0;
    if (saida.formaMudou) d_->lerFormaDoCursor(informacao);

    if (informacao.LastPresentTime.QuadPart == 0) {
        // Só o cursor se moveu: o DXGI sinaliza, mas a imagem é a mesma de
        // antes. Codificar isso de novo seria banda jogada fora.
        liberarQuadro();
        return ResultadoQuadro::SemMudanca;
    }

    if (FAILED(recurso.As(&d_->texturaAtual))) {
        liberarQuadro();
        return ResultadoQuadro::Erro;
    }

    saida.textura = d_->texturaAtual.Get();
    saida.largura = d_->info.largura;
    saida.altura = d_->info.altura;
    saida.quadrosAcumulados = informacao.AccumulatedFrames;
    saida.latenciaUs =
        (agoraQpc() - informacao.LastPresentTime.QuadPart) * 1'000'000 / frequenciaQpc();
    saida.cursorVisivel = d_->cursorVisivel;
    saida.cursorX = d_->cursorX;
    saida.cursorY = d_->cursorY;

    return ResultadoQuadro::Ok;
}

void ScreenCapture::liberarQuadro() {
    if (!d_->quadroEmMaos) return;
    d_->texturaAtual.Reset();
    if (d_->duplicacao) d_->duplicacao->ReleaseFrame();
    d_->quadroEmMaos = false;
}

const FormaCursor& ScreenCapture::formaDoCursor() const { return d_->forma; }

// Converte a forma do cursor para BGRA.
//
// O Windows entrega em três formatos. O colorido já é BGRA. O monocromático são
// duas máscaras de 1 bit empilhadas (AND e XOR) que juntas dizem transparente,
// preto, branco ou inverter — a inversão vira branco aqui, porque inverter o
// que está embaixo exigiria ler o quadro. O mascarado é BGRA onde alfa 0 quer
// dizer "usa o que está embaixo".
void ScreenCapture::Interno::lerFormaDoCursor(const DXGI_OUTDUPL_FRAME_INFO& informacao) {
    bufferForma.resize(informacao.PointerShapeBufferSize);

    DXGI_OUTDUPL_POINTER_SHAPE_INFO infoForma{};
    UINT usado = 0;
    if (FAILED(duplicacao->GetFramePointerShape(informacao.PointerShapeBufferSize,
                                                bufferForma.data(), &usado, &infoForma))) {
        return;
    }

    forma.ancoraX = static_cast<int32_t>(infoForma.HotSpot.x);
    forma.ancoraY = static_cast<int32_t>(infoForma.HotSpot.y);
    forma.largura = infoForma.Width;

    if (infoForma.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        // Altura vem dobrada: as duas máscaras vêm uma embaixo da outra.
        forma.altura = infoForma.Height / 2;
        forma.pixels.assign(static_cast<size_t>(forma.largura) * forma.altura * 4, 0);

        for (uint32_t y = 0; y < forma.altura; ++y) {
            for (uint32_t x = 0; x < forma.largura; ++x) {
                const size_t byte = static_cast<size_t>(y) * infoForma.Pitch + x / 8;
                const uint8_t bit = static_cast<uint8_t>(0x80 >> (x % 8));
                const bool transparente = (bufferForma[byte] & bit) != 0;
                const size_t byteXor =
                    static_cast<size_t>(y + forma.altura) * infoForma.Pitch + x / 8;
                const bool aceso = (bufferForma[byteXor] & bit) != 0;

                uint8_t* destino = &forma.pixels[(static_cast<size_t>(y) * forma.largura + x) * 4];
                if (transparente && !aceso) continue;  // nada
                const uint8_t tom = aceso ? 255 : 0;
                destino[0] = destino[1] = destino[2] = tom;
                destino[3] = 255;
            }
        }
        return;
    }

    // Colorido e mascarado: já vêm em BGRA.
    forma.altura = infoForma.Height;
    forma.pixels.assign(static_cast<size_t>(forma.largura) * forma.altura * 4, 0);
    for (uint32_t y = 0; y < forma.altura; ++y) {
        const uint8_t* origem = bufferForma.data() + static_cast<size_t>(y) * infoForma.Pitch;
        uint8_t* destino = &forma.pixels[static_cast<size_t>(y) * forma.largura * 4];
        std::memcpy(destino, origem, static_cast<size_t>(forma.largura) * 4);
    }

    if (infoForma.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        // Alfa 0 quer dizer "deixa passar"; 255 quer dizer "inverte". Tratar a
        // inversão como opaco é o que os capturadores costumam fazer.
        for (size_t i = 0; i + 3 < forma.pixels.size(); i += 4) {
            forma.pixels[i + 3] = forma.pixels[i + 3] == 0 ? 0 : 255;
        }
    }
}

ID3D11Device* ScreenCapture::dispositivo() const { return d_->dispositivo.Get(); }
ID3D11DeviceContext* ScreenCapture::contexto() const { return d_->contexto.Get(); }
const MonitorInfo& ScreenCapture::monitor() const { return d_->info; }

}  // namespace gl
