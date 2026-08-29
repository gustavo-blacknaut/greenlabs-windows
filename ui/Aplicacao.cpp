#include "ui/Aplicacao.h"

#include <windows.h>

#include <windowsx.h>  // GET_X_LPARAM e GET_Y_LPARAM

#include <d3d11.h>
#include <objbase.h>
#include <wrl/client.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio/AudioCodec.h"
#include "audio/AudioPlayer.h"
#include "capture/AudioCapture.h"
#include "capture/Cursor.h"
#include "config/Config.h"
#include "capture/ProcessTree.h"
#include "capture/ScreenCapture.h"
#include "decoder/VideoDecoder.h"
#include "encoder/VideoEncoder.h"
#include "network/Midia.h"
#include "network/Signaling.h"
#include "ui/Renderizador.h"
#include "ui/Tema.h"
#include "util/Log.h"
#include "video/ColorConverter.h"

// O define vem do CMake como texto comum; a interface desenha em wide.
#define GL_W2(x) L##x
#define GL_W(x) GL_W2(x)
#define GREENLABS_VERSAO_W GL_W(GREENLABS_VERSAO)

using Microsoft::WRL::ComPtr;

namespace gl {
namespace {

// O identificador que o servidor usa quando esta retransmitindo o video.
// Ele participa da sala, mas nao e uma pessoa.
constexpr const char* kIdDoSFU = "sfu";

// A thread do WASAPI é de tempo real: ela não pode alocar, travar nem esperar
// por ninguém. Cada uma dessas coisas estava acontecendo, e o efeito não ficava
// contido no áudio - a captura atrasava, o transporte engasgava, e o ping
// chegou a 679 ms com o servidor no mesmo país.
//
// Daí o anel abaixo, de tamanho fixo e sem trava: a captura escreve num espaço
// que já existe e segue adiante. Quem envia é outra thread.
constexpr size_t kEspacosAudio = 32;
constexpr size_t kMaxPacoteAudio = 1500;

struct PacoteAudio {
    uint8_t dados[kMaxPacoteAudio];
    size_t tamanho = 0;
    int64_t tempoUs = 0;
};


const wchar_t* kClasse = L"GreenLabsJanela";

std::wstring paraW(const std::string& texto) {
    if (texto.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, texto.c_str(), -1, nullptr, 0);
    std::wstring saida(n > 1 ? static_cast<size_t>(n - 1) : 0, L'\0');
    if (n > 1) ::MultiByteToWideChar(CP_UTF8, 0, texto.c_str(), -1, saida.data(), n);
    return saida;
}

std::string paraUtf8(const std::wstring& texto) {
    if (texto.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, texto.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string saida(n > 1 ? static_cast<size_t>(n - 1) : 0, '\0');
    if (n > 1) {
        ::WideCharToMultiByte(CP_UTF8, 0, texto.c_str(), -1, saida.data(), n, nullptr, nullptr);
    }
    return saida;
}

bool dentro(const D2D1_RECT_F& area, float x, float y) {
    return x >= area.left && x <= area.right && y >= area.top && y <= area.bottom;
}

enum class Tela { Entrada, AoVivo };

// Taxa e resolução são fixas de propósito. Deixar o encoder seguir a taxa de
// entrega da duplicação — que varia com o quanto a tela muda — faz o bitrate
// oscilar e o outro lado ver a imagem acelerar e travar. Fixando, cada quadro
// tem o mesmo peso e o controle de bitrate tem um alvo estável.
struct Qualidade {
    const wchar_t* rotulo;
    uint32_t largura;
    uint32_t altura;
    uint32_t fps;
    uint32_t bitrate;
};

constexpr Qualidade kQualidades[] = {
    {L"720p  30fps",  1280,  720, 30, 2'200'000},
    {L"1080p 30fps", 1920, 1080, 30, 4'500'000},
    {L"1080p 60fps", 1920, 1080, 60, 7'500'000},
};

// Campo de texto: guarda o conteúdo e se está com o foco. O desenho fica na
// Aplicacao, que é quem conhece o layout.
struct Campo {
    std::wstring valor;
    std::wstring dica;
    D2D1_RECT_F area{};
    bool focado = false;

    // Ctrl+A não tem seleção parcial aqui: ou tudo está marcado, ou nada. É o
    // que a combinação faz na prática num campo de uma linha - marca tudo para
    // a próxima tecla substituir.
    bool tudoSelecionado = false;
};

}  // namespace

struct Aplicacao::Interno {
    // Texto mostrado quando iniciar() nao chega ao fim.
    std::wstring motivoFalha;

    HWND janela = nullptr;
    Renderizador render;

    ScreenCapture tela;
    Cursor cursor;
    bool cursorPronto = false;
    ColorConverter conversor;
    VideoEncoder encoder;

    // Recepção. O decodificador entrega NV12, que o Direct2D não sabe desenhar,
    // então o mesmo Video Processor faz a volta para BGRA.
    // Uma transmissão que está chegando. Uma por pessoa que transmite.
    //
    // Cada uma tem decodificador próprio porque um decodificador H.264 guarda
    // estado entre quadros - a cadeia de referências. Alimentar o mesmo com
    // dois fluxos diferentes produz lixo, e era por isso que só dava para ver
    // uma pessoa por vez.
    struct Transmissao {
        std::string id;
        std::string nome;
        VideoDecoder decodificador;
        bool decodificadorPronto = false;

        // Rodízio de três, pelo mesmo motivo de sempre: aquele em que o Video
        // Processor escreve nunca é o que a interface está desenhando.
        static constexpr int kBuffers = 3;
        ColorConverter exibicao[kBuffers];
        bool exibicaoPronta[kBuffers] = {false, false, false};
        int exibicaoAtual = 0;

        std::mutex travaQuadro;
        ID3D11Texture2D* quadro = nullptr;
        uint32_t largura = 0;
        uint32_t altura = 0;

        std::mutex travaFila;
        std::vector<std::vector<uint8_t>> fila;

        // Última vez que chegou pacote. Serve para tirar da lista quem parou de
        // transmitir: o servidor não avisa fim de faixa, o fluxo só seca.
        std::chrono::steady_clock::time_point visto{};
    };

    std::mutex travaTransmissoes;
    std::map<std::string, std::unique_ptr<Transmissao>> transmissoes;

    // Qual está no palco. Vazio significa "a primeira que houver".
    std::string noPalco;

    VideoDecoder decodificador;

    // Três conversores em rodízio, e não um.
    //
    // O conversor reaproveita a mesma textura de saída a cada quadro. Com um
    // só, a thread do decodificador escrevia nela com o Video Processor
    // enquanto a thread da interface desenhava a partir dela - o Direct2D
    // então precisa esperar o Blt terminar, e essa espera é uma parada de GPU
    // dentro do laço da interface. Era boa parte do "a tela trava quando os
    // dois rodam juntos": sozinho não havia com quem esperar.
    //
    // Em rodízio, aquele em que o decodificador escreve nunca é o que a
    // interface está lendo. São 8 MB por textura em 1080p; três é barato.

    // Medição do caminho de recepção. Sem isto, "está travando" não diz onde.
    std::atomic<uint64_t> videoChegou{0};
    std::atomic<uint64_t> videoDescartes{0};
    std::atomic<uint64_t> videoPicoFila{0};
    std::atomic<uint64_t> videoDecodado{0};
    std::atomic<uint64_t> videoDecodeUsPico{0};
    std::atomic<uint64_t> videoDecodeUsSoma{0};
    std::chrono::steady_clock::time_point marcaVideo = std::chrono::steady_clock::now();
    AudioCapture audio;
    AudioEncoder audioEnc;
    AudioDecoder audioDec;
    AudioPlayer alto;

    // Reaproveitada a cada pacote de audio: alocar numa thread de tempo real e
    // o tipo de coisa que sai como estalo.
    std::vector<std::shared_ptr<ConexaoPar>> destinosAudio;

    // O envio de audio NAO acontece na thread do WASAPI. Ela e de tempo real:
    // o send faz criptografia e escrita no socket, e fazer isso ali trava a
    // captura (que sai como estalo) e ocupa o transporte (que atrasa o video).
    // A thread de captura so enfileira; quem envia e esta aqui.
    // Anel de tamanho fixo. Escrita e leitura andam sozinhas; quando a escrita
    // alcança a leitura, o pacote mais velho é sobrescrito - áudio atrasado não
    // serve, e esperar por espaço travaria a captura.
    PacoteAudio anelAudio[kEspacosAudio];
    std::atomic<uint64_t> escritaAudio{0};
    std::atomic<uint64_t> leituraAudio{0};

    std::atomic<int64_t> picoAudioUs{0};
    std::atomic<int64_t> somaAudioUs{0};
    std::atomic<uint64_t> chamadasAudio{0};
    std::chrono::steady_clock::time_point marcaAudio = std::chrono::steady_clock::now();
    std::thread threadAudio;
    std::atomic<bool> enviandoAudio{false};
    void lacoEnvioAudio();

    // O mesmo para o video que CHEGA. Decodificar dentro do callback da rede
    // segura a thread que tambem responde ping e trata RTP - com decodificador
    // de software a 1080p sao dezenas de milissegundos por quadro, e o ping
    // aparecia em 679 ms com o servidor no mesmo pais.
    std::mutex travaFilaVideo;
    std::condition_variable temVideo;
    std::thread threadVideo;
    std::atomic<bool> decodificando{false};

    /// Encerra a thread de decodificacao e espera por ela.
    ///
    /// Precisa ser chamada em TODO caminho de saida: uma std::thread destruida
    /// ainda ativa chama std::terminate, e o processo morre com
    /// STATUS_STACK_BUFFER_OVERRUN - um crash na saida, sem nada no log.
    void pararDecodificacao() {
        decodificando.store(false);
        temVideo.notify_all();
        if (threadVideo.joinable()) threadVideo.join();
    }
    void lacoDecodificacao();
    std::vector<std::shared_ptr<ConexaoPar>> destinosVideo;
    Signaling sinal;

    Tela telaAtual = Tela::Entrada;

    Campo campoNome{L"", L"como os outros vão te ver"};
    Campo campoServidor{L"", L"exemplo.com:25640"};
    Campo campoSala{L"call1", L"call1"};

    Config config;
    std::vector<D2D1_RECT_F> btServidores;

    std::vector<MonitorInfo> monitores;
    int monitorEscolhido = 0;
    int qualidadeEscolhida = 1;  // 1080p 30fps

    // Mandar o som do sistema junto com a tela. Só vale para o que sai daqui:
    // o que chega dos outros toca sempre.
    bool audioLigado = true;
    D2D1_RECT_F btAudio{};

    // Volume do que chega da chamada, de 0 a 100.
    int volumeDaChamada = 100;
    D2D1_RECT_F barraVolume{};
    bool arrastandoVolume = false;

    /// Converte a posicao do mouse na barra em volume, e aplica na hora.
    void ajustarVolumePor(float x) {
        const float largura = barraVolume.right - barraVolume.left;
        if (largura <= 0) return;
        const float fracao = (x - barraVolume.left) / largura;
        const int novo = static_cast<int>((fracao < 0 ? 0 : (fracao > 1 ? 1 : fracao)) * 100.0f + 0.5f);
        if (novo == volumeDaChamada) return;
        volumeDaChamada = novo;
        alto.definirVolume(static_cast<float>(novo) / 100.0f);
    }
    bool transmitindo = false;
    std::wstring aviso;

    // O quadro mais recente da captura, para a prévia. A textura é da própria
    // duplicação e vale só até o próximo liberarQuadro().
    // Só a thread de captura toca isto. Ela é dona do quadro da duplicação, e
    // é ela que o libera - por isso a interface nunca desenha a partir daqui.
    ID3D11Texture2D* quadroAtual = nullptr;

    // A prévia que a interface desenha é uma CÓPIA publicada pela thread de
    // captura, em rodízio de dois. Sem a cópia, a interface leria a textura da
    // duplicação enquanto a captura a devolve ao sistema no quadro seguinte -
    // que é ler memória que já não é nossa.
    //
    // CopyResource, e não o ColorConverter: BGRA para BGRA não é conversão
    // nenhuma, e o Video Processor da AMD recusa esse par - o
    // CreateVideoProcessorInputView devolvia E_INVALIDARG a cada quadro e a
    // prévia ficava preta. Cópia é trabalho de CopyResource.
    static constexpr int kBuffersPrevia = 2;
    ComPtr<ID3D11Texture2D> previaTex[kBuffersPrevia];
    uint32_t previaLargura = 0;
    uint32_t previaAltura = 0;
    int previaAtual = 0;
    std::mutex travaPrevia;
    ID3D11Texture2D* quadroPrevia = nullptr;

    // Captura e desenho compartilham a thread da interface - ver o comentário
    // em rodar(). A trava fica porque a recuperação de reset da GPU refaz a
    // captura, e nada pode estar dentro dela nesse momento.
    std::mutex travaCaptura;
    void refazerDispositivo();
    void reiniciarEncode();
    void pararEncodeSomente();

    ID3D11Texture2D* previaDaTela() {
        std::lock_guard t(travaPrevia);
        return quadroPrevia;
    }

    std::mutex travaPares;
    std::vector<Participante> pares;

    // Uma conexão de mídia por participante. Numa sala de N pessoas são N-1:
    // é a topologia em malha, a mesma dos outros clientes.
    std::mutex travaConexoes;
    std::map<std::string, std::shared_ptr<ConexaoPar>> conexoes;
    std::string meuId;
    std::atomic<bool> modoSfu{false};
    std::atomic<bool> conectado{false};

    // Botões, guardados entre o desenho e o clique.
    D2D1_RECT_F btMinimizar{}, btMaximizar{}, btFechar{};
    D2D1_RECT_F btEntrar{}, btTransmitir{}, btSair{};
    std::vector<D2D1_RECT_F> btMonitores;
    std::vector<D2D1_RECT_F> btQualidades;
    std::vector<D2D1_RECT_F> btTransmissoes;
    std::vector<std::string> idsTransmissoes;

    double fps = 0;
    int64_t quadrosNoSegundo = 0;
    std::chrono::steady_clock::time_point marcaFps = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point ultimoEncode = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point ultimoDesenho = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point proximaCaptura = std::chrono::steady_clock::now();

    void desenhar();
    void desenharBarraTitulo();
    void desenharEntrada();
    void desenharAoVivo();
    void clique(float x, float y);
    void tecla(wchar_t c);
    void colar();
    void bombearCaptura();
    bool comecarTransmissao();
    void pararTransmissao();
    void conectar();
    std::shared_ptr<ConexaoPar> abrirMidiaPara(const std::string& peerId);
    void tratarRepasse(const std::string& de, const Json& msg);
    void enviarQuadroParaTodos(const PacoteCodificado& pacote);
    void exibirQuadroDe(Transmissao& quem, ID3D11Texture2D* nv12, uint32_t largura,
                        uint32_t altura);

    Campo* campoFocado();
    void desenharCampo(Campo& campo, const std::wstring& rotulo);
    bool desenharBotao(const D2D1_RECT_F& area, const std::wstring& rotulo, bool destaque,
                       bool habilitado = true);
};

// ---------------------------------------------------------------- ciclo de vida

Aplicacao::Aplicacao() : d_(std::make_unique<Interno>()) {}
Aplicacao::~Aplicacao() = default;

namespace {

LRESULT CALLBACK procedimento(HWND janela, UINT msg, WPARAM w, LPARAM l) {
    auto* d = reinterpret_cast<Aplicacao::Interno*>(::GetWindowLongPtrW(janela, GWLP_USERDATA));

    switch (msg) {
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;

        // A moldura que o Windows desenha por cima da janela.
        //
        // A janela é WS_POPUP | WS_THICKFRAME: o THICKFRAME dá o
        // redimensionamento, mas junto vem uma moldura que o DWM pinta - e é
        // dela a linha clara no topo. Dizer que a área de cliente ocupa a
        // janela inteira faz a moldura sumir; o preço é que o
        // redimensionamento passa a ser nosso, no WM_NCHITTEST abaixo.
        case WM_NCCALCSIZE:
            if (w) {
                // Maximizada é o caso especial: sem descontar a borda, a janela
                // avança por cima da barra de tarefas.
                if (::IsZoomed(janela)) {
                    auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(l);
                    const int bx = ::GetSystemMetrics(SM_CXSIZEFRAME) +
                                   ::GetSystemMetrics(SM_CXPADDEDBORDER);
                    const int by = ::GetSystemMetrics(SM_CYSIZEFRAME) +
                                   ::GetSystemMetrics(SM_CXPADDEDBORDER);
                    p->rgrc[0].left += bx;
                    p->rgrc[0].right -= bx;
                    p->rgrc[0].top += by;
                    p->rgrc[0].bottom -= by;
                }
                return 0;
            }
            return ::DefWindowProcW(janela, msg, w, l);

        // Redimensionamento por conta própria, já que não há mais moldura para
        // o Windows testar. Só as bordas: o miolo continua sendo cliente, e a
        // faixa do título continua arrastando pelo WM_LBUTTONDOWN.
        case WM_NCHITTEST: {
            if (::IsZoomed(janela)) return HTCLIENT;

            RECT r{};
            ::GetWindowRect(janela, &r);
            const int x = GET_X_LPARAM(l);
            const int y = GET_Y_LPARAM(l);
            const int m = 6;  // margem de agarre, em pixels

            const bool esq = x < r.left + m;
            const bool dir = x >= r.right - m;
            const bool cima = y < r.top + m;
            const bool baixo = y >= r.bottom - m;

            if (cima && esq) return HTTOPLEFT;
            if (cima && dir) return HTTOPRIGHT;
            if (baixo && esq) return HTBOTTOMLEFT;
            if (baixo && dir) return HTBOTTOMRIGHT;
            if (cima) return HTTOP;
            if (baixo) return HTBOTTOM;
            if (esq) return HTLEFT;
            if (dir) return HTRIGHT;
            return HTCLIENT;
        }

        case WM_SIZE:
            if (d && w != SIZE_MINIMIZED) {
                d->render.redimensionar(LOWORD(l), HIWORD(l));
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (d) {
                const float x = static_cast<float>(GET_X_LPARAM(l));
                const float y = static_cast<float>(GET_Y_LPARAM(l));
                // A faixa de cima arrasta a janela, como na barra de título do
                // Electron - menos onde há botão.
                if (y < tema::kAlturaTitulo && x < d->render.largura() - 3 * tema::kLarguraBotaoTitulo) {
                    ::ReleaseCapture();
                    ::SendMessageW(janela, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                    return 0;
                }
                d->clique(x, y);
            }
            return 0;

        // Arrastar na barra de volume. Sem estes dois, so daria para clicar num
        // ponto - e ajustar volume e coisa que se faz arrastando e ouvindo.
        case WM_MOUSEMOVE:
            if (d && d->arrastandoVolume && (w & MK_LBUTTON)) {
                d->ajustarVolumePor(static_cast<float>(GET_X_LPARAM(l)));
            }
            return 0;

        case WM_LBUTTONUP:
            if (d && d->arrastandoVolume) {
                d->arrastandoVolume = false;
                ::ReleaseCapture();
                d->config.volume = d->volumeDaChamada;
                d->config.salvar();
            }
            return 0;

        case WM_CHAR:
            if (d) d->tecla(static_cast<wchar_t>(w));
            return 0;

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(l);
            info->ptMinTrackSize.x = 980;
            info->ptMinTrackSize.y = 620;
            return 0;
        }

        default:
            return ::DefWindowProcW(janela, msg, w, l);
    }
}

}  // namespace

const std::wstring& Aplicacao::motivoDaFalha() const { return d_->motivoFalha; }

bool Aplicacao::iniciar(const std::wstring& titulo, int largura, int altura,
                        const Inicial& inicial) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW classe{};
    classe.cbSize = sizeof(classe);
    classe.lpfnWndProc = procedimento;
    classe.hInstance = ::GetModuleHandleW(nullptr);
    classe.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    // O ícone do recurso vira o do arquivo sozinho, mas o da JANELA (barra de
    // tarefas e Alt+Tab) precisa ser dito à classe. Sem isto o Windows põe o
    // ícone genérico de aplicativo.
    classe.hIcon = ::LoadIconW(classe.hInstance, MAKEINTRESOURCEW(1));
    classe.hIconSm = classe.hIcon;
    classe.lpszClassName = kClasse;
    // Sem pincel de fundo: quem pinta é o Direct2D, e deixar o Windows apagar
    // antes causa piscada a cada redimensionamento.
    classe.hbrBackground = nullptr;
    ::RegisterClassExW(&classe);

    d_->janela = ::CreateWindowExW(0, kClasse, titulo.c_str(),
                                   WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
                                   CW_USEDEFAULT, CW_USEDEFAULT, largura, altura, nullptr, nullptr,
                                   classe.hInstance, nullptr);
    if (!d_->janela) {
        erro("nao foi possivel criar a janela");
        d_->motivoFalha = L"Nao foi possivel criar a janela do aplicativo.";
        return false;
    }
    ::SetWindowLongPtrW(d_->janela, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d_.get()));

    // Preferências guardadas: ninguém quer digitar o endereço do servidor
    // toda vez que abre o aplicativo.
    d_->config = Config::carregar();
    d_->campoNome.valor = paraW(d_->config.nome);
    d_->campoSala.valor = paraW(d_->config.sala);
    d_->campoServidor.valor = paraW(d_->config.servidor);
    d_->qualidadeEscolhida =
        (d_->config.qualidade >= 0 && d_->config.qualidade < 3) ? d_->config.qualidade : 1;
    d_->audioLigado = d_->config.audio;
    d_->volumeDaChamada = (d_->config.volume >= 0 && d_->config.volume <= 100) ? d_->config.volume : 100;
    d_->alto.definirVolume(static_cast<float>(d_->volumeDaChamada) / 100.0f);

    d_->monitores = ScreenCapture::listarMonitores();
    if (d_->monitores.empty()) {
        erro("nenhum monitor encontrado");
        d_->motivoFalha =
            L"Nenhum monitor ligado a placa de video foi encontrado.\n\n"
            L"Isso costuma acontecer com acesso remoto ou com a tela desconectada.";
        return false;
    }

    // Falha aqui e do dispositivo D3D11, nao da duplicacao: a captura em si
    // pode nao estar disponivel agora e o aplicativo continua funcionando.
    if (!d_->tela.iniciar(0)) {
        d_->motivoFalha =
            L"Nao foi possivel preparar o Direct3D 11 no monitor principal.";
        return false;
    }
    if (!d_->render.iniciar(d_->janela, d_->tela.dispositivo())) {
        d_->motivoFalha =
            L"Nao foi possivel iniciar o desenho da interface (Direct2D).";
        return false;
    }

    if (!d_->tela.capturando()) {
        aviso("comecando sem captura de tela; sera aberta quando der");
    }

    Signaling::Ouvintes ouvintes;
    // Saida de audio e decodificador de pe antes de qualquer pacote chegar.
    // Abrir COM e criar thread dentro do callback da rede e trabalho pesado no
    // pior lugar possivel.
    //
    // Sem condicao: o botao de audio decide o que EU mando, nao o que eu ouco.
    // Desligar o proprio microfone do sistema e uma escolha; deixar de ouvir
    // quem esta do outro lado nunca foi pedido por ninguem.
    d_->audioDec.iniciar();
    d_->alto.iniciar();

    ouvintes.aoSaberDoModo = [this](bool sfu) {
        d_->modoSfu.store(sfu);
        if (sfu) info("servidor retransmitindo: nao vou oferecer para os participantes");
    };
    ouvintes.aoEntrar = [this](const std::string& eu, const std::vector<Participante>& pares) {
        {
            std::lock_guard trava(d_->travaPares);
            d_->meuId = eu;
            d_->pares = pares;
        }
        d_->conectado.store(true);
        d_->telaAtual = Tela::AoVivo;

        // Chegamos numa sala que já tinha gente: nós é que oferecemos. Em modo
        // retransmissor, não: quem negocia mídia é o servidor, e oferecer
        // direto fazia a mesma tela chegar duas vezes do outro lado - uma pelo
        // servidor, que funciona, e outra direta, que falha e fica preta.
        if (d_->transmitindo && !d_->modoSfu.load()) {
            for (const auto& p : pares) {
                if (auto conexao = d_->abrirMidiaPara(p.id)) conexao->oferecer();
            }
        }
    };
    ouvintes.aoChegarAlguem = [this](const Participante& p) {
        {
            std::lock_guard trava(d_->travaPares);
            d_->pares.push_back(p);
        }
        // Quem CHEGA é quem oferece — é o que o cliente web e o de Electron
        // fazem, e seguir a mesma regra evita os dois lados oferecerem ao mesmo
        // tempo. Aqui só preparamos a conexão com a faixa pronta, para poder
        // responder à oferta que vem em seguida.
        if (d_->transmitindo && !d_->modoSfu.load()) {
            d_->abrirMidiaPara(p.id);
            d_->encoder.pedirQuadroChave();
        }
    };
    ouvintes.aoSairAlguem = [this](const std::string& id) {
        {
            std::lock_guard trava(d_->travaPares);
            std::erase_if(d_->pares, [&](const Participante& p) { return p.id == id; });
        }
        std::lock_guard trava(d_->travaConexoes);
        d_->conexoes.erase(id);
    };

    ouvintes.aoRepasse = [this](const std::string& de, const Json& msg) {
        d_->tratarRepasse(de, msg);
    };
    ouvintes.aoCair = [this](const std::string& motivo) {
        d_->conectado.store(false);
        d_->aviso = L"A conexão caiu: " + paraW(motivo);
        d_->telaAtual = Tela::Entrada;
    };
    d_->sinal.definirOuvintes(std::move(ouvintes));

    if (!inicial.nome.empty()) d_->campoNome.valor = paraW(inicial.nome);
    if (!inicial.sala.empty()) d_->campoSala.valor = paraW(inicial.sala);
    if (!inicial.servidor.empty()) {
        d_->campoServidor.valor = paraW(inicial.servidor);
        d_->conectar();
        if (inicial.transmitirJa) d_->comecarTransmissao();
    }

    ::ShowWindow(d_->janela, SW_SHOW);
    return true;
}

// Refaz tudo que estava preso ao dispositivo D3D11 depois de um reset da GPU.
//
// Roda na thread da interface, com a captura travada: nada pode estar dentro do
// encoder ou da duplicação enquanto os dois são recriados.
void Aplicacao::Interno::refazerDispositivo() {
    gl::aviso("a GPU foi reiniciada; refazendo o video");

    const bool estavaTransmitindo = transmitindo;
    pararEncodeSomente();

    {
        std::lock_guard travaTela(travaCaptura);

        // Tudo que guarda ponteiro do dispositivo velho some primeiro. Deixar
        // qualquer um vivo faz a recriação falhar por referência pendurada.
        quadroAtual = nullptr;
        {
            std::lock_guard t(travaPrevia);
            quadroPrevia = nullptr;
        }
        for (auto& t : previaTex) t.Reset();
        previaLargura = 0;
        previaAltura = 0;
        cursorPronto = false;

        decodificando.store(false);
        temVideo.notify_all();
        if (threadVideo.joinable()) threadVideo.join();
        decodificador.parar();

        {
            std::lock_guard t(travaTransmissoes);
            transmissoes.clear();
            noPalco.clear();
        }

        render.liberar();
        if (!tela.iniciar(static_cast<uint32_t>(monitorEscolhido))) {
            erro("nao foi possivel refazer a captura depois do reset da GPU");
            return;
        }
        if (!render.iniciar(janela, tela.dispositivo())) {
            erro("nao foi possivel refazer a interface depois do reset da GPU");
            return;
        }
    }

    // A thread do decodificador renasce sozinha no próximo quadro que chegar:
    // é o mesmo caminho da primeira vez.
    if (estavaTransmitindo) comecarTransmissao();
    info("video refeito depois do reset da GPU");
}

int Aplicacao::rodar() {
    MSG msg{};
    for (;;) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                d_->pararTransmissao();
                d_->pararDecodificacao();
                d_->sinal.sair();
                return static_cast<int>(msg.wParam);
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }

        // Captura na thread da interface, e é de propósito.
        //
        // Já tentei numa thread separada, que é o desenho mais bonito. Não
        // funciona aqui: SetMultithreadProtected torna cada CHAMADA D3D
        // atômica, mas não torna uma SEQUÊNCIA atômica. O compositor do cursor
        // faz OMSetRenderTargets seguido de Draw; o Direct2D faz a sequência
        // dele. Em threads diferentes sobre o mesmo contexto imediato as duas
        // se intercalam, o estado do pipeline vira lixo, e o driver pendura a
        // GPU - DXGI_ERROR_DEVICE_HUNG em poucos segundos, sem nem haver
        // conexão aberta.
        //
        // Fazer certo com thread separada exigiria contexto próprio para cada
        // uma, ou uma trava em volta de cada sequência. Enquanto isso não
        // existe, as duas ficam aqui: bombearCaptura já respeita o fps
        // escolhido, então isto custa 30 passagens por segundo, não mais.
        d_->bombearCaptura();

        // A GPU pode ser reiniciada pelo Windows a qualquer momento - driver
        // que trava, atualização, jogo pesado abrindo. Quando isso acontece,
        // TODO objeto preso ao dispositivo antigo morre junto, e antes o
        // programa ficava batendo num dispositivo que não existia mais,
        // enchendo o log e mostrando uma tela parada. Refazer é o certo, e é o
        // que o navegador faz.
        if (d_->render.dispositivoPerdido()) d_->refazerDispositivo();

        // Redesenho limitado a 60 por segundo.
        //
        // Antes o laço desenhava tudo de novo a cada volta, e como o Present
        // espera o vsync, num monitor de 144 Hz isso eram 144 repinturas
        // completas da interface por segundo. Fora de gastar GPU à toa, cada
        // volta é uma volta que o decodificador de vídeo e a thread de áudio
        // não têm — é boa parte do que se via como tela e som travando.
        //
        // 60 Hz é o teto do que o olho aproveita numa interface, e o vídeo
        // recebido vem a 30. O resto era desperdício.
        const auto agora = std::chrono::steady_clock::now();
        if (agora - d_->ultimoDesenho >= std::chrono::microseconds(1'000'000 / 60)) {
            d_->ultimoDesenho = agora;
            d_->desenhar();
        } else {
            // Sem nada a desenhar, devolve a fatia em vez de girar em falso.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

// ---------------------------------------------------------------- captura

void Aplicacao::Interno::bombearCaptura() {
    // Ritmo fixo, e o ritmo vale para a captura inteira - não só para o encode.
    //
    // Aqui estava o pior gargalo do programa. Esta função era chamada a cada
    // volta do laço principal, e só o encode respeitava o fps escolhido: o
    // AcquireNextFrame e a composição do cursor rodavam sempre. Com a tela
    // mudando, a duplicação devolve quadro na hora, então o laço girava solto
    // martelando o dispositivo D3D centenas de vezes por segundo para produzir
    // 30 quadros úteis.
    //
    // E o dispositivo é um só: a captura, o conversor, o encoder, o
    // decodificador do vídeo que chega e o desenho da interface compartilham
    // ele. Com a proteção multithread ligada (que o decodificador exige para
    // usar a GPU), toda chamada D3D passa por uma trava única. Transmitir
    // sozinho ia bem, receber sozinho ia bem, e os dois juntos se atropelavam
    // nessa trava - que é exatamente o que se via como tela e áudio travando.
    //
    // Capturando no ritmo certo, o dispositivo fica livre o resto do tempo.
    //
    // Cadência agendada, e não "tempo decorrido desde a última". A diferença
    // decide o fps: este laço roda a cada ~16,6 ms porque o desenho espera o
    // vsync, e perguntar "já passaram 33,3 ms?" a cada 16,6 ms responde 33,3 ou
    // 50 conforme a fase - o que dava os 17 quadros por segundo em vez de 30.
    //
    // Somando o intervalo ao alvo anterior, a cadência não escorrega. O ajuste
    // no fim é para quando a máquina atrasa de verdade: sem ele, o alvo ficaria
    // no passado e a captura tentaria recuperar em rajada.
    const int alvoFps = transmitindo ? kQualidades[qualidadeEscolhida].fps : 30;
    const auto intervalo = std::chrono::microseconds(1'000'000 / alvoFps);
    const auto agoraCaptura = std::chrono::steady_clock::now();

    if (agoraCaptura < proximaCaptura) return;
    proximaCaptura += intervalo;
    if (proximaCaptura < agoraCaptura) proximaCaptura = agoraCaptura + intervalo;

    std::lock_guard travaTela(travaCaptura);

    if (quadroAtual) {
        tela.liberarQuadro();
        quadroAtual = nullptr;
    }

    QuadroCapturado quadro;
    // Prazo curto: a interface não pode ficar esperando a tela mudar, senão
    // clique e digitação engasgam.
    switch (tela.proximoQuadro(4, quadro)) {
        case ResultadoQuadro::Ok: {
            quadrosNoSegundo += 1;

            // O cursor entra aqui, antes de qualquer outra coisa: assim ele
            // aparece tanto na prévia quanto no que é transmitido, e ninguém
            // precisa se perguntar por que a seta some do outro lado.
            if (!cursorPronto) {
                const auto& m = tela.monitor();
                cursorPronto = cursor.iniciar(tela.dispositivo(), tela.contexto(),
                                              m.larguraFisica(), m.alturaFisica());
            }
            if (cursorPronto && quadro.formaMudou) cursor.definirForma(tela.formaDoCursor());

            quadroAtual = cursorPronto ? cursor.compor(quadro.textura, quadro.cursorVisivel,
                                                       quadro.cursorX, quadro.cursorY)
                                       : quadro.textura;

            // Publica a cópia para a interface desenhar. Rodízio de dois pelo
            // mesmo motivo do vídeo recebido: aquele em que se escreve nunca é
            // o que está sendo lido.
            if (quadroAtual) {
                D3D11_TEXTURE2D_DESC origem{};
                quadroAtual->GetDesc(&origem);

                if (previaLargura != origem.Width || previaAltura != origem.Height) {
                    for (auto& t : previaTex) t.Reset();
                    {
                        std::lock_guard t(travaPrevia);
                        quadroPrevia = nullptr;
                    }
                    previaLargura = origem.Width;
                    previaAltura = origem.Height;
                }

                previaAtual = (previaAtual + 1) % kBuffersPrevia;
                ComPtr<ID3D11Texture2D>& destino = previaTex[previaAtual];

                if (!destino) {
                    D3D11_TEXTURE2D_DESC nova = origem;
                    nova.Usage = D3D11_USAGE_DEFAULT;
                    nova.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    nova.CPUAccessFlags = 0;
                    nova.MiscFlags = 0;
                    nova.MipLevels = 1;
                    nova.ArraySize = 1;
                    tela.dispositivo()->CreateTexture2D(&nova, nullptr, &destino);
                }

                if (destino) {
                    tela.contexto()->CopyResource(destino.Get(), quadroAtual);
                    std::lock_guard t(travaPrevia);
                    quadroPrevia = destino.Get();
                }
            }

            if (transmitindo) {
                // Sem segundo limitador: a função inteira já entra no ritmo do
                // fps escolhido. Limitar de novo aqui, com o mesmo intervalo,
                // só faria perder um quadro sim outro não por arredondamento.
                ultimoEncode = agoraCaptura;
                if (auto* nv12 = conversor.converter(quadroAtual)) {
                    encoder.codificar(nv12,
                                      std::chrono::duration_cast<std::chrono::microseconds>(
                                          agoraCaptura.time_since_epoch())
                                          .count());
                }
            }
            break;
        }
        case ResultadoQuadro::PrecisaReiniciar:
            tela.reiniciar();
            break;
        default:
            break;
    }

    if (audioLigado && chamadasAudio.load() > 0) {
        const auto agoraA = std::chrono::steady_clock::now();
        if (agoraA - marcaAudio >= std::chrono::seconds(5)) {
            marcaAudio = agoraA;
            info("audio na thread de tempo real: pico {} us, media {} us, {} chamadas",
                 picoAudioUs.load(),
                 somaAudioUs.load() / static_cast<int64_t>(chamadasAudio.load()),
                 chamadasAudio.load());
        }
    }

    // Relatório do caminho de recepção, de dois em dois segundos enquanto
    // chega vídeo. "Está travando" não diz onde trava; isto diz.
    const auto agoraVideo = std::chrono::steady_clock::now();
    if (videoChegou.load() > 0 && agoraVideo - marcaVideo >= std::chrono::seconds(2)) {
        const double seg = std::chrono::duration<double>(agoraVideo - marcaVideo).count();
        const uint64_t chegou = videoChegou.exchange(0);
        const uint64_t decodou = videoDecodado.exchange(0);
        const uint64_t soma = videoDecodeUsSoma.exchange(0);
        info("video recebido: {:.1f}/s chegando, {:.1f}/s decodando | fila pico {} | "
             "decode media {} us, pico {} us | descartes {}",
             chegou / seg, decodou / seg, videoPicoFila.exchange(0),
             decodou ? soma / decodou : 0, videoDecodeUsPico.exchange(0),
             videoDescartes.exchange(0));
        marcaVideo = agoraVideo;

        if (alto.ativo()) {
            const auto a = alto.estatisticas();
            info("audio recebido: fila {} ms | velocidade {:.4f} | faltas {} | recargas {}",
                 a.filaMs, a.velocidade, a.faltas, a.recargas);
        }
    }

    const auto agora = std::chrono::steady_clock::now();
    const auto decorrido = std::chrono::duration<double>(agora - marcaFps).count();
    if (decorrido >= 1.0) {
        fps = quadrosNoSegundo / decorrido;
        quadrosNoSegundo = 0;
        marcaFps = agora;
    }
}

bool Aplicacao::Interno::comecarTransmissao() {
    // A trava da captura vale para tudo que a thread de captura usa - e ela usa
    // o conversor e o encoder, não só a duplicação. Sem isto, clicar em entrar
    // recriava o encoder enquanto a outra thread estava dentro dele, e o
    // aplicativo parava ali mesmo.
    //
    // Só a thread da interface chama esta função, e nenhuma chamada vem de
    // dentro da própria trava: a troca de monitor solta antes de chegar aqui.
    std::lock_guard travaTela(travaCaptura);

    const auto& m = tela.monitor();
    const Qualidade& q = kQualidades[qualidadeEscolhida];

    // A ENTRADA é o tamanho físico da textura, não o lógico.
    //
    // Num monitor girado para retrato o DXGI diz 1080x1920 (o que a pessoa vê),
    // mas entrega uma textura 1920x1080 com o conteúdo deitado. Passar o lógico
    // aqui era pedir ao Video Processor para ler algo que não existe - e o
    // resultado era a tela chegando virada do outro lado.
    const uint32_t entradaL = m.larguraFisica();
    const uint32_t entradaA = m.alturaFisica();

    // A SAÍDA respeita a proporção da tela, dentro do que a qualidade permite.
    //
    // Antes eu esticava qualquer tela para o tamanho fixo do preset, então um
    // monitor em pé era espremido para 16:9 e chegava achatado. Aqui a escala é
    // a mesma nos dois eixos, e o que sobra é resolução, não deformação.
    const double escala = std::min(static_cast<double>(q.largura) / m.largura,
                                   static_cast<double>(q.altura) / m.altura);
    const uint32_t saidaL = static_cast<uint32_t>(m.largura * escala) & ~1u;
    const uint32_t saidaA = static_cast<uint32_t>(m.altura * escala) & ~1u;

    if (!conversor.iniciar(tela.dispositivo(), tela.contexto(), entradaL, entradaA, saidaL, saidaA,
                           ColorConverter::Saida::Nv12, m.graus)) {
        aviso = L"Não foi possível preparar a conversão de cor.";
        return false;
    }

    ConfigEncoder cfg;
    cfg.largura = conversor.largura();
    cfg.altura = conversor.altura();
    cfg.fps = q.fps;
    cfg.bitrate = q.bitrate;

    if (!encoder.iniciar(tela.dispositivo(), cfg,
                         [this](const PacoteCodificado& pacote) {
                             enviarQuadroParaTodos(pacote);
                         })) {
        aviso = L"Não foi possível iniciar o encoder H.264.";
        return false;
    }
    encoder.pedirQuadroChave();

    // Áudio: tudo menos o Discord.
    const uint32_t excluir = acharRaizParaExcluir(
        {"discord", "discordptb", "discordcanary", "discorddevelopment"});
    // O som do sistema, sem o Discord, vira Opus e sai junto do video. Roda em
    // thread de tempo real: nada de alocar nem travar aqui - o codificador
    // reaproveita os proprios buffers justamente por isso.
    if (audioLigado) audioEnc.iniciar();
    if (audioLigado) {
        audio.iniciar(excluir, [this](const float* pcm, uint32_t quadros) {
            const auto t0 = std::chrono::steady_clock::now();

            // Drena TODOS os quadros fechados, não só um.
            //
            // O WASAPI não entrega em fatias de 20 ms: numa volta ele pode
            // trazer 40 ou 60. Emitindo um pacote por chamada, o resto ficava
            // acumulado e a captura ia ficando para trás um quadro por vez, até
            // o acumulador estourar e descartar tudo de uma vez. Era isso o
            // "está perdendo áudio quando manda", e piorava justamente sob
            // carga - que é quando se está transmitindo vídeo.
            audioEnc.acumular(pcm, quadros);

            const int64_t agora = std::chrono::duration_cast<std::chrono::microseconds>(
                                      t0.time_since_epoch())
                                      .count();

            for (;;) {
                const auto& pacote = audioEnc.proximo();
                if (pacote.empty()) break;

                // Só copiar para o anel e publicar o índice. Enviar daqui -
                // SRTP mais socket - é trabalho pesado numa thread de tempo
                // real, e era o que fazia a captura engasgar. Quem envia é a
                // threadAudio, que existe para isso.
                const uint64_t w = escritaAudio.load(std::memory_order_relaxed);
                PacoteAudio& espaco = anelAudio[w % kEspacosAudio];
                const size_t cabem =
                    pacote.size() < kMaxPacoteAudio ? pacote.size() : kMaxPacoteAudio;
                memcpy(espaco.dados, pacote.data(), cabem);
                espaco.tamanho = cabem;
                espaco.tempoUs = agora;
                escritaAudio.store(w + 1, std::memory_order_release);
            }

            const auto gasto = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - t0)
                                   .count();
            somaAudioUs.fetch_add(gasto, std::memory_order_relaxed);
            chamadasAudio.fetch_add(1, std::memory_order_relaxed);
            if (gasto > picoAudioUs.load(std::memory_order_relaxed)) {
                picoAudioUs.store(gasto, std::memory_order_relaxed);
            }
        });

        // A thread que realmente envia. Ela existia e nunca era iniciada: o
        // anel inteiro era código morto e o áudio saía da thread de tempo real.
        escritaAudio.store(0);
        leituraAudio.store(0);
        enviandoAudio.store(true);
        threadAudio = std::thread([this] { lacoEnvioAudio(); });
    }

    // Quem já está na sala precisa receber a oferta agora; quem chegar depois
    // recebe no aoChegarAlguem.
    std::vector<Participante> agora;
    {
        std::lock_guard trava(travaPares);
        agora = pares;
    }
    for (const auto& p : agora) {
        if (auto conexao = abrirMidiaPara(p.id)) conexao->oferecer();
    }

    transmitindo = true;
    aviso.clear();
    return true;
}

void Aplicacao::Interno::pararTransmissao() {
    // Mesma trava do comecarTransmissao, e pela mesma razão: encoder.parar()
    // enquanto a thread de captura está codificando é destruir o objeto debaixo
    // de quem o usa. A ordem é sempre travaCaptura antes de travaConexoes, aqui
    // e na thread de captura - por isso não há como travar uma na outra.
    std::lock_guard travaTela(travaCaptura);

    if (!transmitindo) return;
    transmitindo = false;
    if (audioLigado && chamadasAudio.load() > 0) {
        info("audio na thread de tempo real: pico {} us, media {} us em {} chamadas",
             picoAudioUs.load(), somaAudioUs.load() / static_cast<int64_t>(chamadasAudio.load()),
             chamadasAudio.load());
    }
    audio.parar();
    enviandoAudio.store(false);
    if (threadAudio.joinable()) threadAudio.join();
    escritaAudio.store(0);
    leituraAudio.store(0);

    encoder.parar();
    audioEnc.parar();
    // As duas listas TAMBEM: elas guardam shared_ptr das conexoes, entao
    // limpar so o mapa deixava tudo vivo por causa delas - o servidor nunca via
    // a transmissao terminar. Foi efeito do cache que eu criei para tirar o
    // envio de dentro da trava.
    destinosAudio.clear();
    destinosVideo.clear();
    {
        std::lock_guard trava(travaConexoes);
        conexoes.clear();
    }
}

// Troca a fonte da imagem SEM derrubar a conexão.
//
// Trocar de monitor, de qualidade ou o som ligado/desligado só precisa refazer
// o encoder e o conversor. Antes tudo isso passava por pararTransmissao, que
// também limpa o mapa de conexões - ou seja, mudar de monitor destruía a
// conexão com o servidor e obrigava uma renegociação inteira. Cruzando os dois
// logs dava para ver: "midia com sfu: fechado" no instante da troca, e
// "falhou" um minuto depois, quando a renegociação não completava. O
// travamento que parecia do servidor era isto.
//
// Resolução nova não precisa de renegociação: o H.264 anuncia o tamanho no
// próprio fluxo, pelo SPS, e o decodificador do outro lado se reconfigura
// sozinho. Só é preciso mandar um quadro-chave logo em seguida.
void Aplicacao::Interno::pararEncodeSomente() {
    if (!transmitindo) return;

    // Este bloco vivia, por engano, dentro do callback de tempo real do áudio -
    // dava para ver no commit be30630. Lá ele derrubava a thread de envio e a
    // de decodificação no primeiro pacote depois de conectar, e aqui não havia
    // desmonte nenhum: as threads vazavam a cada transmissão.
    //
    // A captura de som para PRIMEIRO. Enquanto ela roda, o callback continua
    // escrevendo no anel, e parar o consumidor antes do produtor deixaria o
    // anel enchendo sozinho.
    audio.parar();

    enviandoAudio.store(false);
    if (threadAudio.joinable()) threadAudio.join();

    std::lock_guard travaTela(travaCaptura);
    encoder.parar();
    audioEnc.parar();
    escritaAudio.store(0);
    leituraAudio.store(0);
    transmitindo = false;
}

void Aplicacao::Interno::reiniciarEncode() {
    if (!transmitindo) return;
    pararEncodeSomente();
    comecarTransmissao();
}

void Aplicacao::Interno::conectar() {
    const std::string servidor = paraUtf8(campoServidor.valor);
    if (servidor.empty()) {
        aviso = L"Informe o endereço do servidor.";
        return;
    }
    aviso = L"Conectando...";

    config.servidor = servidor;
    config.sala = paraUtf8(campoSala.valor);
    config.nome = paraUtf8(campoNome.valor);
    config.qualidade = qualidadeEscolhida;
    config.audio = audioLigado;
    config.volume = volumeDaChamada;
    config.monitor = monitorEscolhido;
    config.lembrarServidor(servidor);
    config.salvar();

    // Conexões da sessão anterior não valem nada aqui, e uma delas atrapalha
    // de verdade: o id do servidor é sempre "sfu", então na reconexão a
    // conexão velha - já estável - continuava no mapa. A oferta nova caía nela
    // e era recusada com "Unexpected local description type answer in
    // signaling state stable". Com participante comum isso nunca aparecia,
    // porque o id deles muda a cada entrada.
    {
        std::lock_guard trava(travaConexoes);
        conexoes.clear();
    }

    if (!sinal.entrar(servidor, paraUtf8(campoSala.valor), paraUtf8(campoNome.valor))) {
        aviso = L"Não foi possível conectar. Confira o endereço e se o servidor está no ar.";
        return;
    }
    aviso.clear();
}

// ---------------------------------------------------------------- midia

std::shared_ptr<ConexaoPar> Aplicacao::Interno::abrirMidiaPara(const std::string& peerId) {
    // Em modo retransmissor existe UMA conexao de midia: a do servidor.
    // Abrir conexao direta com as pessoas fazia a mesma tela chegar duas vezes
    // do outro lado - uma pelo servidor, que funciona, e outra direta, que
    // costuma falhar e fica preta. A guarda fica aqui, no funil por onde toda
    // conexao nasce, e nao em cada chamador: espalhada, sempre escapa uma.
    if (modoSfu.load() && peerId != kIdDoSFU) return nullptr;

    {
        std::lock_guard trava(travaConexoes);
        auto achou = conexoes.find(peerId);
        if (achou != conexoes.end()) return achou->second;
    }

    const Qualidade& q = kQualidades[qualidadeEscolhida];
    ConfigMidia cfg;
    cfg.largura = q.largura;
    cfg.altura = q.altura;
    cfg.fps = q.fps;
    cfg.bitrate = q.bitrate;

    // O servidor em modo SFU tem endereço público: basta o nosso candidato
    // refletido pelo STUN para o caminho fechar. Pedir TURN aqui só adiciona
    // uma espera que já custou a conexão inteira - a coleta demorava 24
    // segundos e o servidor desistia aos 30.
    // TURN ligado tambem contra o servidor. Desliguei achando que so
    // atrasava - e atrasa mesmo a coleta - mas no br-02 o caminho direto nao
    // fecha, e o relay e o unico que funciona: o Electron conecta la porque
    // tem TURN, e o C++ nao conectava porque eu tinha tirado.
    cfg.usarTurn = (peerId != kIdDoSFU);

    auto conexao = std::make_shared<ConexaoPar>(peerId, cfg);

    // O cliente web le "sdp" e o de Electron le "description". Mandar os dois
    // faz o mesmo pacote servir para os dois sem eles precisarem mudar.
    conexao->aoDescrever([this, peerId](const std::string& tipo, const std::string& sdp) {
        Json descricao = Json::objeto();
        descricao["type"] = Json{tipo};
        descricao["sdp"] = Json{sdp};

        Json msg = Json::objeto();
        msg["type"] = Json{tipo};
        msg["sdp"] = descricao;
        msg["description"] = descricao;
        sinal.enviarPara(peerId, msg);
    });

    // Quando o outro lado pede quadro-chave, quem produz é o encoder.
    conexao->aoPedirChave([this] { encoder.pedirQuadroChave(); });

    // Vídeo que chega. O decodificador é um só para a sala inteira: com o
    // servidor retransmitindo, quem transmite por vez é uma pessoa, e abrir um
    // decodificador por conexão gastaria GPU à toa.
    conexao->aoReceberAudio([this](const std::byte* dados, size_t tamanho) {
        if (!audioDec.ativo() || !alto.ativo()) return;

        const auto& pcm = audioDec.decodificar(reinterpret_cast<const uint8_t*>(dados), tamanho);
        if (!pcm.empty()) alto.enfileirar(pcm.data(), static_cast<uint32_t>(pcm.size() / 2));
    });

    conexao->aoReceberVideo([this, peerId](const std::byte* dados, size_t tamanho,
                                           const std::string& faixaId) {
        if (!decodificando.load()) {
            decodificando.store(true);
            threadVideo = std::thread([this] { lacoDecodificacao(); });
        }

        // Uma transmissão por faixa.
        //
        // O faixaId sempre chegou aqui e era jogado fora com um (void). Era por
        // isso que só dava para ver uma pessoa: tudo caía numa fila só e num
        // decodificador só, e duas pessoas transmitindo viravam um fluxo
        // embaralhado. Com uma entrada por faixa, cada uma tem a sua fila, o
        // seu decodificador e a sua imagem.
        Transmissao* t = nullptr;
        {
            std::lock_guard trava(travaTransmissoes);
            auto& espaco = transmissoes[faixaId];
            if (!espaco) {
                espaco = std::make_unique<Transmissao>();
                espaco->id = faixaId;
                info("nova transmissao na sala: faixa {}", faixaId.substr(0, 24));
            }
            t = espaco.get();

            // Em modo retransmissor o peerId é sempre "sfu", então o nome vem
            // da lista de participantes: quem está na sala e não somos nós.
            if (peerId == kIdDoSFU) {
                std::lock_guard tp(travaPares);
                if (t->nome.empty()) {
                    for (const auto& p : pares) {
                        if (p.id != meuId) { t->nome = p.nome; break; }
                    }
                    if (t->nome.empty()) t->nome = "Alguem na sala";
                }
            } else if (t->nome.empty()) {
                t->nome = peerId;
            }
            t->visto = std::chrono::steady_clock::now();
        }

        {
            std::lock_guard tf(t->travaFila);
            // H.264 não admite descarte no meio. Cada quadro P só faz sentido
            // a partir do anterior, então jogar fora o mais antigo corrompe
            // tudo até o próximo keyframe — e é exatamente isso que se via
            // como a tela congelando.
            //
            // Se a fila enche, o decodificador parou de valer: limpa tudo de
            // uma vez e espera o próximo keyframe — um corte só, em vez de
            // corrupção contínua.
            if (t->fila.size() >= 30) {
                t->fila.clear();
                videoDescartes.fetch_add(1, std::memory_order_relaxed);
            }
            videoChegou.fetch_add(1, std::memory_order_relaxed);
            if (t->fila.size() > videoPicoFila.load(std::memory_order_relaxed)) {
                videoPicoFila.store(t->fila.size(), std::memory_order_relaxed);
            }
            t->fila.emplace_back(reinterpret_cast<const uint8_t*>(dados),
                                 reinterpret_cast<const uint8_t*>(dados) + tamanho);
        }
        temVideo.notify_one();
    });

    conexao->aoCandidato([this, peerId](const std::string& candidato, const std::string& mid) {
        Json corpo = Json::objeto();
        corpo["candidate"] = Json{candidato};
        corpo["sdpMid"] = Json{mid};
        corpo["sdpMLineIndex"] = Json{0};

        Json msg = Json::objeto();
        msg["type"] = Json{"ice"};
        msg["candidate"] = corpo;
        sinal.enviarPara(peerId, msg);
    });

    // A faixa NÃO é criada aqui de propósito. Quando somos nós que
    // respondemos, ela nasce da oferta do outro lado (pelo onTrack) e já vem
    // com o mid certo. Criar uma nossa antes fazia o onTrack ser ignorado, e a
    // faixa com mid proprio nao casava com nenhuma m-line da oferta: a conexao
    // ficava de pe e nenhum quadro saia.
    {
        std::lock_guard trava(travaConexoes);
        conexoes[peerId] = conexao;
    }
    return conexao;
}

void Aplicacao::Interno::tratarRepasse(const std::string& de, const Json& msg) {
    const std::string tipo = msg.texto("type");

    std::shared_ptr<ConexaoPar> conexao;
    {
        std::lock_guard trava(travaConexoes);
        auto achou = conexoes.find(de);
        if (achou != conexoes.end()) conexao = achou->second;
    }

    // Colisão: os dois lados ofereceram ao mesmo tempo. Aplicar a oferta do
    // outro por cima da nossa dá "Incompatible roles" e a conexão morre ali.
    //
    // A regra é a mesma dos outros clientes: quem tem o id maior cede. Como o
    // libdatachannel não desfaz uma descrição local, ceder significa recriar a
    // conexão do zero — e é o que acontece aqui.
    if (conexao && tipo == "offer" && conexao->ofertaPendente()) {
        // Contra o servidor não há desempate: ele é quem renegocia para a sala
        // inteira, e uma oferta nossa que vença a dele deixa a mídia sem sair
        // do lugar. O desempate por id ainda por cima favorecia o lado errado
        // - "sfu" é maior que qualquer UUID em ordem de texto, então nós
        // sempre mantínhamos a nossa e ignorávamos a dele.
        if (de == kIdDoSFU || de < meuId) {
            gl::aviso("colisao de ofertas com {}: cedendo", de.substr(0, 8));
            {
                std::lock_guard trava(travaConexoes);
                conexoes.erase(de);
            }
            conexao = abrirMidiaPara(de);
        } else {
            gl::aviso("colisao de ofertas com {}: mantendo a nossa", de.substr(0, 8));
            return;
        }
    }

    // Oferta de alguém com quem ainda não há conexão: cria e responde.
    //
    // Sem isto o cliente só sabia falar, nunca ouvir - e quando o outro lado
    // oferecia primeiro (o que acontece quando ele entra na sala depois de
    // nós), a oferta era jogada fora em silêncio. Ele ficava esperando resposta
    // que nunca vinha, e quando nós ofereciamos depois ele ignorava por
    // colisão. Nenhum dos dois conectava, e o resultado era tela preta com
    // "transmitindo" na interface.
    if (!conexao && tipo == "offer") {
        conexao = abrirMidiaPara(de);
    }
    if (!conexao) return;

    if (tipo == "answer" || tipo == "offer") {
        // De novo os dois nomes de campo, porque os dois clientes usam nomes
        // diferentes para a mesma coisa.
        const Json& corpo = msg.tem("sdp") ? msg.filho("sdp") : msg.filho("description");
        const std::string sdp = corpo.ehObjeto() ? corpo.texto("sdp") : msg.texto("sdp");
        if (!sdp.empty()) conexao->receberDescricao(tipo, sdp);
        return;
    }

    if (tipo == "ice") {
        const Json& corpo = msg.filho("candidate");
        const std::string candidato =
            corpo.ehObjeto() ? corpo.texto("candidate") : msg.texto("candidate");
        const std::string mid = corpo.ehObjeto() ? corpo.texto("sdpMid", "0") : "0";
        if (!candidato.empty()) conexao->receberCandidato(candidato, mid);
    }
}

// exibirQuadroRecebido converte o quadro decodado e guarda para o desenho.
//
// Chamado da thread da rede, e o desenho acontece na thread da janela - por
// isso a trava. Guarda só o ponteiro: a textura pertence ao conversor e vale
// até o próximo quadro, que é o comportamento que a interface quer de qualquer
// forma (ela desenha o mais recente).
void Aplicacao::Interno::exibirQuadroDe(Transmissao& quem, ID3D11Texture2D* nv12,
                                        uint32_t largura, uint32_t altura) {
    if (!nv12 || largura == 0 || altura == 0) return;

    // Passa para o próximo do rodízio antes de escrever: assim o Video
    // Processor nunca escreve na textura que a interface está desenhando.
    quem.exibicaoAtual = (quem.exibicaoAtual + 1) % Transmissao::kBuffers;
    ColorConverter& conversor = quem.exibicao[quem.exibicaoAtual];
    bool& pronto = quem.exibicaoPronta[quem.exibicaoAtual];

    if (!pronto || conversor.largura() != largura || conversor.altura() != altura) {
        if (!conversor.iniciar(tela.dispositivo(), tela.contexto(), largura, altura, largura,
                               altura, ColorConverter::Saida::Bgra)) {
            return;
        }
        pronto = true;
        if (quem.exibicaoAtual == 0) {
            info("recebendo video de {}: {}x{}", quem.nome, largura, altura);
        }
    }

    ID3D11Texture2D* bgra = conversor.converter(nv12);
    if (!bgra) return;

    std::lock_guard trava(quem.travaQuadro);
    quem.quadro = bgra;
    quem.largura = largura;
    quem.altura = altura;
}

// Uma thread só, servindo todas as transmissões por vez.
//
// Uma thread por transmissão seria o desenho óbvio, e é o errado aqui: todas
// usariam o mesmo dispositivo D3D11, e já custou caro descobrir que sequências
// D3D de threads diferentes se intercalam e penduram a GPU. Com uma thread só,
// a concorrência sobre o dispositivo continua sendo a mesma de sempre - esta
// thread e a da interface - por mais gente que entre na sala.
void Aplicacao::Interno::lacoDecodificacao() {
    std::vector<uint8_t> quadro;
    std::vector<Transmissao*> lista;

    while (decodificando.load()) {
        {
            std::unique_lock t(travaFilaVideo);
            temVideo.wait_for(t, std::chrono::milliseconds(20),
                              [this] { return !decodificando.load(); });
        }
        if (!decodificando.load()) break;

        lista.clear();
        {
            std::lock_guard t(travaTransmissoes);
            for (auto& [id, quem] : transmissoes) lista.push_back(quem.get());
        }

        bool fezAlgo = false;
        for (Transmissao* quem : lista) {
            // O decodificador nasce na primeira vez que esta transmissão tem
            // quadro. Criar antes gastaria GPU por alguém que talvez nunca
            // transmita.
            if (!quem->decodificadorPronto) {
                if (!quem->decodificador.iniciar(
                        tela.dispositivo(),
                        [this, quem](ID3D11Texture2D* nv12, uint32_t l, uint32_t a) {
                            exibirQuadroDe(*quem, nv12, l, a);
                        })) {
                    continue;
                }
                quem->decodificadorPronto = true;
            }

            // No máximo alguns quadros por volta, para uma transmissão atrasada
            // não deixar as outras paradas.
            for (int i = 0; i < 4; ++i) {
                {
                    std::lock_guard tf(quem->travaFila);
                    if (quem->fila.empty()) break;
                    quadro.swap(quem->fila.front());
                    quem->fila.erase(quem->fila.begin());
                }
                fezAlgo = true;

                const auto antes = std::chrono::steady_clock::now();
                quem->decodificador.decodificar(
                    quadro.data(), quadro.size(),
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        antes.time_since_epoch())
                        .count());
                const auto gasto = std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - antes)
                                       .count();
                videoDecodado.fetch_add(1, std::memory_order_relaxed);
                videoDecodeUsSoma.fetch_add(static_cast<uint64_t>(gasto),
                                            std::memory_order_relaxed);
                if (static_cast<uint64_t>(gasto) >
                    videoDecodeUsPico.load(std::memory_order_relaxed)) {
                    videoDecodeUsPico.store(static_cast<uint64_t>(gasto),
                                            std::memory_order_relaxed);
                }
            }
        }
        (void)fezAlgo;

        // Quem parou de mandar sai da lista. O servidor não avisa fim de faixa;
        // o fluxo simplesmente seca, e sem isto o cartão ficaria para sempre na
        // tela mostrando o último quadro de alguém que já saiu.
        const auto agora = std::chrono::steady_clock::now();
        std::lock_guard t(travaTransmissoes);
        for (auto it = transmissoes.begin(); it != transmissoes.end();) {
            if (it->second && agora - it->second->visto > std::chrono::seconds(5)) {
                info("transmissao encerrada: {}", it->second->nome);
                render.esquecerVideo(it->first);
                if (noPalco == it->first) noPalco.clear();
                it = transmissoes.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void Aplicacao::Interno::lacoEnvioAudio() {
    uint8_t pacote[kMaxPacoteAudio];
    size_t tamanho = 0;
    int64_t tempo = 0;

    while (enviandoAudio.load()) {
        const uint64_t fim = escritaAudio.load(std::memory_order_acquire);
        uint64_t inicio = leituraAudio.load(std::memory_order_relaxed);

        if (inicio == fim) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Ficou para trás mais que o anel: pula para o mais recente. Mandar
        // áudio velho só aumenta o atraso de quem ouve.
        if (fim - inicio > kEspacosAudio) inicio = fim - 1;

        {
            const PacoteAudio& espaco = anelAudio[inicio % kEspacosAudio];
            tamanho = espaco.tamanho;
            tempo = espaco.tempoUs;
            memcpy(pacote, espaco.dados, tamanho);
        }
        leituraAudio.store(inicio + 1, std::memory_order_relaxed);

        {
            std::lock_guard trava(travaConexoes);
            if (destinosAudio.size() != conexoes.size()) {
                destinosAudio.clear();
                destinosAudio.reserve(conexoes.size());
                for (auto& [id, conexao] : conexoes) {
                    if (conexao) destinosAudio.push_back(conexao);
                }
            }
        }
        for (auto& conexao : destinosAudio) {
            conexao->enviarAudio(pacote, tamanho, tempo);
        }
    }
}

void Aplicacao::Interno::enviarQuadroParaTodos(const PacoteCodificado& pacote) {
    // Um encoder, N transportes. Codificar por destinatario multiplicaria o
    // custo pelo tamanho da sala e derrubaria a maquina numa chamada grande.
    // Envio FORA da trava, pelo mesmo motivo do audio: segurar travaConexoes
    // durante o envio poe as duas midias uma na frente da outra.
    {
        std::lock_guard trava(travaConexoes);
        if (destinosVideo.size() != conexoes.size()) {
            destinosVideo.clear();
            destinosVideo.reserve(conexoes.size());
            for (auto& [id, conexao] : conexoes) {
                if (conexao) destinosVideo.push_back(conexao);
            }
        }
    }
    for (auto& conexao : destinosVideo) {
        conexao->enviarVideo(pacote.dados, pacote.tamanho, pacote.tempoUs, pacote.chave);
    }
}

// ---------------------------------------------------------------- entrada

Campo* Aplicacao::Interno::campoFocado() {
    if (campoNome.focado) return &campoNome;
    if (campoServidor.focado) return &campoServidor;
    if (campoSala.focado) return &campoSala;
    return nullptr;
}

// Ctrl+V. O WM_CHAR entrega 0x16 para essa combinação, e sem tratar isso
// simplesmente não dava para colar o endereço do servidor - que é justamente o
// texto que ninguém quer digitar na mão.
void Aplicacao::Interno::colar() {
    Campo* campo = campoFocado();
    if (!campo || !::OpenClipboard(janela)) return;

    if (campo->tudoSelecionado) {
        campo->valor.clear();
        campo->tudoSelecionado = false;
    }

    if (HANDLE dados = ::GetClipboardData(CF_UNICODETEXT)) {
        if (auto* texto = static_cast<const wchar_t*>(::GlobalLock(dados))) {
            for (const wchar_t* p = texto; *p && campo->valor.size() < 200; ++p) {
                // Endereço copiado de um chat costuma vir com quebra de linha
                // grudada; deixá-la entrar quebraria a conexão sem explicação.
                if (*p >= 32) campo->valor.push_back(*p);
            }
            ::GlobalUnlock(dados);
        }
    }
    ::CloseClipboard();
}

void Aplicacao::Interno::tecla(wchar_t c) {
    if (c == 0x16) {  // Ctrl+V
        colar();
        return;
    }
    if (c == 0x01) {  // Ctrl+A
        if (Campo* campo = campoFocado()) campo->tudoSelecionado = !campo->valor.empty();
        return;
    }
    Campo* campo = campoFocado();
    if (!campo) return;

    // Com tudo marcado, qualquer tecla que produza texto substitui o conteúdo -
    // e Backspace apaga tudo de uma vez. É o comportamento que a pessoa espera
    // depois de um Ctrl+A.
    if (campo->tudoSelecionado && (c == L'\b' || c >= 32)) {
        campo->valor.clear();
        campo->tudoSelecionado = false;
        if (c == L'\b') return;
    }

    if (c == L'\b') {
        if (!campo->valor.empty()) campo->valor.pop_back();
    } else if (c == L'\t' || c == L'\r') {
        // Enter na tela de entrada vale como clicar em entrar.
        if (telaAtual == Tela::Entrada && c == L'\r') conectar();
    } else if (c >= 32) {
        if (campo->valor.size() < 120) campo->valor.push_back(c);
    }
}

void Aplicacao::Interno::clique(float x, float y) {
    if (dentro(btFechar, x, y)) {
        ::PostMessageW(janela, WM_CLOSE, 0, 0);
        return;
    }
    if (dentro(btMinimizar, x, y)) {
        ::ShowWindow(janela, SW_MINIMIZE);
        return;
    }
    if (dentro(btMaximizar, x, y)) {
        const bool maximizada = ::IsZoomed(janela);
        ::ShowWindow(janela, maximizada ? SW_RESTORE : SW_MAXIMIZE);
        return;
    }

    if (telaAtual == Tela::Entrada) {
        for (Campo* campo : {&campoNome, &campoServidor, &campoSala}) {
            campo->focado = dentro(campo->area, x, y);
            if (!campo->focado) campo->tudoSelecionado = false;
        }
        for (size_t i = 0; i < btServidores.size() && i < config.servidores.size(); ++i) {
            if (dentro(btServidores[i], x, y)) {
                campoServidor.valor = paraW(config.servidores[i]);
                campoServidor.tudoSelecionado = false;
                return;
            }
        }
        if (dentro(btEntrar, x, y)) conectar();
        return;
    }

    for (size_t i = 0; i < btMonitores.size(); ++i) {
        if (dentro(btMonitores[i], x, y)) {
            const int novo = static_cast<int>(i);
            if (novo != monitorEscolhido) {
                const bool estavaTransmitindo = transmitindo;
                // So o encode: a conexao com o servidor continua de pe. Antes
                // isto chamava pararTransmissao, que limpa o mapa de conexoes -
                // trocar de monitor derrubava a chamada inteira e a
                // renegociacao seguinte falhava.
                pararEncodeSomente();

                // O renderizador vive no dispositivo D3D11 da captura. Trocar de
                // monitor destrói esse dispositivo, então ele precisa soltar
                // tudo ANTES - senão fica com uma cadeia de troca apontando para
                // um dispositivo morto, e a nova criação falha em silêncio
                // porque o DXGI só aceita uma cadeia por janela.
                render.liberar();
                cursorPronto = false;

                monitorEscolhido = novo;
                // Trocar o monitor troca o objeto de duplicação por baixo da
                // thread de captura. Sem esta trava ela estaria adquirindo
                // quadro de um objeto que deixou de existir no meio da chamada.
                {
                    std::lock_guard travaTela(travaCaptura);
                    if (quadroAtual) {
                        tela.liberarQuadro();
                        quadroAtual = nullptr;
                    }
                    {
                        std::lock_guard t(travaPrevia);
                        quadroPrevia = nullptr;
                    }
                    for (auto& t : previaTex) t.Reset();
                    previaLargura = 0;
                    previaAltura = 0;
                    cursorPronto = false;

                    // O decodificador também: ele nasceu amarrado ao dispositivo
                    // D3D antigo, e trocar de monitor cria um novo. Sem derrubar
                    // aqui, ele seguia decodificando para uma GPU que não é mais
                    // a nossa - e a tela de quem estava transmitindo
                    // simplesmente não aparecia mais depois da troca.
                    decodificando.store(false);
                    temVideo.notify_all();
                    if (threadVideo.joinable()) threadVideo.join();
                    decodificador.parar();
                    {
                        std::lock_guard t(travaTransmissoes);
                        transmissoes.clear();
                        noPalco.clear();
                    }

                    if (!tela.iniciar(static_cast<uint32_t>(novo))) {
                        aviso = L"Não foi possível capturar esse monitor.";
                        tela.iniciar(0);
                        monitorEscolhido = 0;
                    }
                }
                if (!render.iniciar(janela, tela.dispositivo())) {
                    aviso = L"A troca de monitor falhou. Reabra o aplicativo.";
                    return;
                }
                if (estavaTransmitindo) comecarTransmissao();
            }
            return;
        }
    }

    // Cartao de transmissao: poe aquela no palco.
    for (size_t i = 0; i < btTransmissoes.size() && i < idsTransmissoes.size(); ++i) {
        if (dentro(btTransmissoes[i], x, y)) {
            std::lock_guard t(travaTransmissoes);
            noPalco = idsTransmissoes[i];
            return;
        }
    }

    for (size_t i = 0; i < btQualidades.size(); ++i) {
        if (dentro(btQualidades[i], x, y)) {
            const int novo = static_cast<int>(i);
            if (novo != qualidadeEscolhida) {
                qualidadeEscolhida = novo;
                // Refaz só o encoder: a conexão continua de pé. Ver
                // reiniciarEncode().
                reiniciarEncode();
            }
            return;
        }
    }

    // A barra de volume vem antes dos botoes: ela ocupa a largura toda do
    // painel, e um clique nela nao pode cair em outro controle.
    if (dentro(barraVolume, x, y)) {
        arrastandoVolume = true;
        ::SetCapture(janela);
        ajustarVolumePor(x);
        return;
    }

    if (dentro(btAudio, x, y)) {
        audioLigado = !audioLigado;
        config.audio = audioLigado;
    config.volume = volumeDaChamada;
        config.salvar();
        // Mesma regra da qualidade: a captura de som nasce junto com a
        // transmissão, então trocar no meio recomeça - sem derrubar a conexão.
        reiniciarEncode();
        return;
    }

    if (dentro(btTransmitir, x, y)) {
        if (transmitindo) pararTransmissao();
        else comecarTransmissao();
        return;
    }
    if (dentro(btSair, x, y)) {
        pararTransmissao();
        sinal.sair();
        conectado.store(false);
        telaAtual = Tela::Entrada;
    }
}

// ---------------------------------------------------------------- desenho

bool Aplicacao::Interno::desenharBotao(const D2D1_RECT_F& area, const std::wstring& rotulo,
                                       bool destaque, bool habilitado) {
    const auto fundo = destaque ? tema::kVerdeSuave : tema::kPainel2;
    const auto borda = destaque ? tema::kVerdeLinha : tema::kLinha;
    const auto corTexto = !habilitado ? tema::kApagado : (destaque ? tema::kVerde : tema::kTexto);

    render.retangulo(area, fundo, tema::kRaioBotao);
    render.contorno(area, borda, tema::kRaioBotao);
    render.texto(rotulo, area, corTexto, Fonte::Botao, DWRITE_TEXT_ALIGNMENT_CENTER);
    return habilitado;
}

void Aplicacao::Interno::desenharCampo(Campo& campo, const std::wstring& rotulo) {
    render.texto(rotulo, D2D1::RectF(campo.area.left, campo.area.top - 22, campo.area.right,
                                     campo.area.top - 4),
                 tema::kApagado, Fonte::Pequena);

    render.retangulo(campo.area, tema::kPainel2, tema::kRaioBotao);
    render.contorno(campo.area, campo.focado ? tema::kVerdeLinha : tema::kLinha, tema::kRaioBotao);

    const auto interna = D2D1::RectF(campo.area.left + 14, campo.area.top, campo.area.right - 14,
                                     campo.area.bottom);
    if (campo.valor.empty()) {
        render.texto(campo.dica, interna, tema::kApagado, Fonte::Corpo);
    } else {
        // Cursor piscando só no campo com foco: sem isso não dá para saber onde
        // a digitação vai cair.
        if (campo.tudoSelecionado) {
            // Fundo verde no texto marcado, para o Ctrl+A ter efeito visível.
            const float larguraTexto = render.larguraDoTexto(campo.valor, Fonte::Corpo);
            render.retangulo(D2D1::RectF(interna.left - 3, interna.top + 9,
                                         interna.left + larguraTexto + 3, interna.bottom - 9),
                             tema::kVerdeSuave, 4);
        }
        const bool piscar = campo.focado && !campo.tudoSelecionado &&
                            (::GetTickCount64() / 500) % 2 == 0;
        render.texto(campo.valor + (piscar ? L"|" : L""), interna, tema::kTexto, Fonte::Corpo);
    }
}

void Aplicacao::Interno::desenharBarraTitulo() {
    const float larg = render.largura();
    render.retangulo(D2D1::RectF(0, 0, larg, tema::kAlturaTitulo), tema::kPainel);
    render.linha(0, tema::kAlturaTitulo, larg, tema::kAlturaTitulo, tema::kLinha);

    render.logo(D2D1::RectF(12, 7, 12 + 24, tema::kAlturaTitulo - 7));
    render.texto(L"GreenLabs", D2D1::RectF(44, 0, 220, tema::kAlturaTitulo), tema::kTexto,
                 Fonte::Botao);
    render.texto(L"v" GREENLABS_VERSAO_W L"  nativo", D2D1::RectF(124, 0, 280, tema::kAlturaTitulo), tema::kApagado,
                 Fonte::Pequena);

    const float b = tema::kLarguraBotaoTitulo;
    btFechar = D2D1::RectF(larg - b, 0, larg, tema::kAlturaTitulo);
    btMaximizar = D2D1::RectF(larg - 2 * b, 0, larg - b, tema::kAlturaTitulo);
    btMinimizar = D2D1::RectF(larg - 3 * b, 0, larg - 2 * b, tema::kAlturaTitulo);

    render.texto(L"—", btMinimizar, tema::kApagado, Fonte::Botao, DWRITE_TEXT_ALIGNMENT_CENTER);
    render.texto(::IsZoomed(janela) ? L"❐" : L"□", btMaximizar, tema::kApagado,
                 Fonte::Botao, DWRITE_TEXT_ALIGNMENT_CENTER);
    render.texto(L"✕", btFechar, tema::kVermelho, Fonte::Botao, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void Aplicacao::Interno::desenharEntrada() {
    const float larg = render.largura();
    const float alt = render.altura();

    const float largCartao = 520;
    const float altCartao = 460;
    const float x = (larg - largCartao) / 2;
    const float y = (alt - altCartao) / 2 + tema::kAlturaTitulo / 2;

    render.retangulo(D2D1::RectF(x, y, x + largCartao, y + altCartao), tema::kPainel,
                     tema::kRaioCartao);
    render.contorno(D2D1::RectF(x, y, x + largCartao, y + altCartao), tema::kLinha,
                    tema::kRaioCartao);

    render.logo(D2D1::RectF(x + 36, y + 26, x + 36 + 56, y + 82));
    render.texto(L"SEM CONTA · SEM LIMITE DE TEMPO",
                 D2D1::RectF(x + 104, y + 34, x + largCartao - 36, y + 54), tema::kVerde,
                 Fonte::Pequena);
    render.texto(L"Entrar numa sala",
                 D2D1::RectF(x + 104, y + 54, x + largCartao - 36, y + 92), tema::kTexto,
                 Fonte::Subtitulo);

    const float larguraCampo = largCartao - 72;
    campoNome.area = D2D1::RectF(x + 36, y + 140, x + 36 + larguraCampo, y + 186);
    campoServidor.area = D2D1::RectF(x + 36, y + 222, x + 36 + larguraCampo, y + 268);
    campoSala.area = D2D1::RectF(x + 36, y + 304, x + 36 + larguraCampo, y + 350);

    desenharCampo(campoNome, L"SEU APELIDO");
    desenharCampo(campoServidor, L"SERVIDOR");
    desenharCampo(campoSala, L"SALA");

    btEntrar = D2D1::RectF(x + 36, y + 380, x + 36 + larguraCampo, y + 428);
    desenharBotao(btEntrar, L"ENTRAR NA SALA", true);

    // Servidores já usados, como atalho. Clicar preenche o campo.
    btServidores.clear();
    if (!config.servidores.empty()) {
        float linha = y + altCartao + 16;
        render.texto(L"SERVIDORES SALVOS",
                     D2D1::RectF(x + 36, linha, x + largCartao - 36, linha + 16), tema::kApagado,
                     Fonte::Pequena);
        linha += 24;

        for (const auto& endereco : config.servidores) {
            const auto area = D2D1::RectF(x + 36, linha, x + largCartao - 36, linha + 34);
            btServidores.push_back(area);

            const bool atual = paraUtf8(campoServidor.valor) == endereco;
            render.retangulo(area, atual ? tema::kVerdeSuave : tema::kPainel2, tema::kRaioBotao);
            render.contorno(area, atual ? tema::kVerdeLinha : tema::kLinha, tema::kRaioBotao);
            render.texto(paraW(endereco),
                         D2D1::RectF(area.left + 14, area.top, area.right - 14, area.bottom),
                         atual ? tema::kVerde : tema::kTexto, Fonte::Pequena);
            linha += 40;
        }
    }

    if (!aviso.empty()) {
        render.texto(aviso, D2D1::RectF(x + 36, y + altCartao + 12, x + largCartao - 36,
                                        y + altCartao + 40),
                     tema::kVermelho, Fonte::Pequena, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

// A tela ao vivo.
//
// Regras do desenho, para as próximas mudanças não desfazerem o que já foi
// resolvido aqui:
//
// - O palco manda. Ele fica com toda a largura e altura que sobrarem; controle
//   nenhum rouba espaço dele.
// - Um item selecionado tem fundo verde OU contorno, nunca os dois. Ter os dois
//   em cada linha era o que deixava o painel visualmente barulhento.
// - Medida de linha é 30 px, não 40. Cabe mais coisa sem apertar nada.
// - O que é diagnóstico fica embaixo, discreto, e só aparece transmitindo.
void Aplicacao::Interno::desenharAoVivo() {
    const float larg = render.largura();
    const float alt = render.altura();
    const float topo = tema::kAlturaTitulo + tema::kEspaco;
    const float painelX = larg - tema::kLarguraPainelLateral - tema::kEspaco;

    // Fotografia das transmissões deste quadro. Copiar o essencial sob trava e
    // desenhar fora dela: o desenho é longo e a trava é disputada com a thread
    // que decodifica.
    struct NaTela {
        std::string id;
        std::wstring nome;
        ID3D11Texture2D* quadro;
        uint32_t largura;
    };
    std::vector<NaTela> aoVivo;
    {
        std::lock_guard trava(travaTransmissoes);
        for (auto& [id, quem] : transmissoes) {
            if (!quem) continue;
            std::lock_guard tq(quem->travaQuadro);
            aoVivo.push_back({id, paraW(quem->nome), quem->quadro, quem->largura});
        }
        // Palco vazio ou apontando para quem saiu: fica com a primeira que há.
        if (!aoVivo.empty()) {
            const bool valido = std::any_of(aoVivo.begin(), aoVivo.end(),
                                            [&](const NaTela& n) { return n.id == noPalco; });
            if (!valido) noPalco = aoVivo.front().id;
        } else {
            noPalco.clear();
        }
    }

    const NaTela* escolhida = nullptr;
    for (const auto& n : aoVivo) {
        if (n.id == noPalco) { escolhida = &n; break; }
    }
    ID3D11Texture2D* doOutro = escolhida ? escolhida->quadro : nullptr;
    const uint32_t doOutroLargura = escolhida ? escolhida->largura : 0;

    // ---- palco
    //
    // Sem barra inferior: os botões foram para o pé do painel lateral, e o
    // palco herdou os 64 px que ela ocupava. Numa tela de chamada, imagem é o
    // conteúdo e botão é moldura.
    const auto palco = D2D1::RectF(tema::kEspaco, topo, painelX - tema::kEspaco,
                                   alt - tema::kEspaco);
    // Palco mais escuro que o painel, e não igual.
    //
    // A imagem entra com a proporção dela, então sobra faixa em volta. Com o
    // palco na mesma cor do painel lateral, essa faixa lia como "a interface
    // está quebrada aqui". Escuro, ela lê como moldura - que é o que é.
    render.retangulo(palco, tema::kFundo, tema::kRaioCartao);

    // Passar textura nula é de propósito: sem quadro novo o renderizador
    // repinta o último. Antes a prévia apagava a cada instante em que a tela
    // não mudava, e o resultado era piscar sem parar.
    const auto dentroDoPalco =
        D2D1::RectF(palco.left + 5, palco.top + 5, palco.right - 5, palco.bottom - 5);
    if (escolhida) {
        render.video(escolhida->id, escolhida->quadro, dentroDoPalco);
    } else {
        render.video("previa", previaDaTela(), dentroDoPalco);
    }

    if (!render.temQuadro(escolhida ? escolhida->id : std::string("previa"))) {
        // Estado vazio com logo e duas linhas: uma dizendo o que está
        // acontecendo, outra dizendo o que fazer. Só a primeira deixava a
        // pessoa parada olhando para um retângulo preto sem saída.
        const float meio = (palco.top + palco.bottom) / 2.0f;
        render.logo(D2D1::RectF(palco.left, meio - 92, palco.right, meio - 28), 0.22f);

        const wchar_t* titulo;
        const wchar_t* dica;
        if (transmitindo) {
            titulo = L"Preparando sua transmissão";
            dica = L"Os outros já vão começar a ver.";
        } else if (!conectado.load()) {
            titulo = L"Você não está numa sala";
            dica = L"Entre numa sala para ver e ser visto.";
        } else {
            titulo = L"Ninguém está transmitindo";
            dica = L"Clique em TRANSMITIR para mostrar a sua tela.";
        }

        render.texto(titulo, D2D1::RectF(palco.left, meio - 14, palco.right, meio + 14),
                     tema::kTexto, Fonte::Subtitulo, DWRITE_TEXT_ALIGNMENT_CENTER);
        render.texto(dica, D2D1::RectF(palco.left, meio + 18, palco.right, meio + 42),
                     tema::kApagado, Fonte::Pequena, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    render.contorno(palco, tema::kLinha, tema::kRaioCartao);

    // Uma etiqueta só, no alto à esquerda, dizendo o que se está vendo. Antes
    // havia duas - selo de ao vivo em cima e nome embaixo - e as duas competiam
    // com a imagem.
    {
        const bool vendoOutro = doOutro && doOutroLargura > 0;
        std::wstring texto;
        D2D1_COLOR_F corTexto = tema::kApagado;

        if (vendoOutro) {
            texto = L"●  " + (escolhida ? escolhida->nome : L"Alguem na sala");
            if (doOutroLargura > 0) texto += L"  ·  " + std::to_wstring(doOutroLargura) + L"p";
            corTexto = tema::kVerde;
        } else if (transmitindo) {
            texto = L"●  AO VIVO  ·  sua tela";
            corTexto = tema::kVerde;
        } else {
            texto = L"sua tela";
        }

        // Ancorada no VÍDEO, não no palco.
        //
        // A imagem quase nunca preenche o palco: ela entra com a proporção
        // certa e sobra faixa preta dos lados ou em cima. Ancorando no palco, a
        // etiqueta ia parar no meio dessa faixa, solta, longe da imagem que ela
        // descreve. Ancorada no vídeo, ela senta no canto da imagem.
        const std::string chave = escolhida ? escolhida->id : std::string("previa");
        const D2D1_RECT_F v = render.areaDoVideo(chave);
        const bool temVideoNaTela = render.temQuadro(chave) && v.right > v.left;
        const float ancoraX = (temVideoNaTela ? v.left : palco.left) + 14;
        const float ancoraY = (temVideoNaTela ? v.top : palco.top) + 14;

        const float largura = render.larguraDoTexto(texto, Fonte::Pequena) + 28;
        const auto etiqueta =
            D2D1::RectF(ancoraX, ancoraY, ancoraX + largura, ancoraY + 30);
        render.retangulo(etiqueta, tema::kFundo, 15);
        if (corTexto.g > 0.5f) render.contorno(etiqueta, tema::kVerdeLinha, 15);
        render.texto(texto, etiqueta, corTexto, Fonte::Pequena, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // ---- painel lateral
    const auto painel = D2D1::RectF(painelX, topo, larg - tema::kEspaco, alt - tema::kEspaco);
    render.retangulo(painel, tema::kPainel, tema::kRaioCartao);
    render.contorno(painel, tema::kLinha, tema::kRaioCartao);

    const float esq = painel.left + 16;
    const float dir = painel.right - 16;
    float y = painel.top + 16;

    auto secao = [&](const wchar_t* titulo) {
        render.texto(titulo, D2D1::RectF(esq, y, dir, y + 14), tema::kApagado, Fonte::Pequena);
        y += 20;
    };

    // Item de lista: fundo quando escolhido, nada quando não. Sem contorno em
    // cima do fundo - é o que tirava o ar do painel.
    auto item = [&](const std::wstring& esquerda, const std::wstring& direita, bool ativo) {
        const auto area = D2D1::RectF(esq, y, dir, y + 30);
        render.retangulo(area, ativo ? tema::kVerdeSuave : tema::kPainel2, 8);
        render.texto(esquerda, D2D1::RectF(area.left + 11, area.top, area.right - 70, area.bottom),
                     ativo ? tema::kVerde : tema::kTexto, Fonte::Pequena);
        if (!direita.empty()) {
            render.texto(direita,
                         D2D1::RectF(area.left, area.top, area.right - 11, area.bottom),
                         tema::kApagado, Fonte::Pequena, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        y += 34;
        return area;
    };

    secao(L"MONITOR");
    btMonitores.clear();
    for (size_t i = 0; i < monitores.size(); ++i) {
        // "Monitor 1" em vez de "\\.\DISPLAY2": o caminho do dispositivo não
        // diz nada a quem está escolhendo onde a tela vai ser capturada.
        const std::wstring nome = L"Monitor " + std::to_wstring(i + 1);
        const std::wstring tamanho = std::to_wstring(monitores[i].largura) + L"×" +
                                     std::to_wstring(monitores[i].altura);
        btMonitores.push_back(item(nome, tamanho, static_cast<int>(i) == monitorEscolhido));
    }

    y += 6;
    secao(L"QUALIDADE");
    btQualidades.clear();
    for (size_t i = 0; i < std::size(kQualidades); ++i) {
        btQualidades.push_back(item(kQualidades[i].rotulo,
                                    std::to_wstring(kQualidades[i].bitrate / 1000) + L" kbps",
                                    static_cast<int>(i) == qualidadeEscolhida));
    }

    // Interruptor do som. Fica logo abaixo da qualidade porque é a mesma
    // decisão - o que sai daqui - mas com forma diferente, porque é liga/desliga
    // e não escolha entre opções.
    y += 6;
    btAudio = D2D1::RectF(esq, y, dir, y + 34);
    render.retangulo(btAudio, audioLigado ? tema::kVerdeSuave : tema::kPainel2, 8);
    render.texto(audioLigado ? L"Som do sistema" : L"Sem som",
                 D2D1::RectF(btAudio.left + 11, btAudio.top, btAudio.right - 64, btAudio.bottom),
                 audioLigado ? tema::kVerde : tema::kApagado, Fonte::Pequena);
    {
        const float tl = btAudio.right - 51;
        const float tt = btAudio.top + 9;
        render.retangulo(D2D1::RectF(tl, tt, tl + 36, tt + 16),
                         audioLigado ? tema::kVerde : tema::kLinha, 8);
        const float bola = audioLigado ? tl + 22 : tl + 2;
        render.retangulo(D2D1::RectF(bola, tt + 2, bola + 12, tt + 14), tema::kPainel, 6);
    }
    y += 40;

    // Volume do que CHEGA da chamada.
    //
    // Fica logo abaixo do interruptor de propósito: os dois falam de som, mas
    // de lados opostos - um é o que sai daqui, o outro é o que entra. E é uma
    // barra, e não um número: ninguém sabe dizer se quer 70 ou 80, mas todo
    // mundo sabe arrastar até parar de incomodar.
    {
        render.texto(L"Volume de quem eu ouço",
                     D2D1::RectF(esq, y, dir - 44, y + 16), tema::kApagado, Fonte::Pequena);
        render.texto(std::to_wstring(volumeDaChamada) + L"%",
                     D2D1::RectF(esq, y, dir, y + 16), tema::kTexto, Fonte::Pequena,
                     DWRITE_TEXT_ALIGNMENT_TRAILING);
        y += 22;

        barraVolume = D2D1::RectF(esq, y, dir, y + 18);
        const float trilhoY = y + 7;
        render.retangulo(D2D1::RectF(esq, trilhoY, dir, trilhoY + 5), tema::kLinha, 3);

        const float fracao = static_cast<float>(volumeDaChamada) / 100.0f;
        const float ate = esq + (dir - esq) * fracao;
        if (ate > esq) render.retangulo(D2D1::RectF(esq, trilhoY, ate, trilhoY + 5), tema::kVerde, 3);

        // A bolinha fica presa dentro da barra nas pontas, senao ela some
        // metade para fora no 0 e no 100.
        const float centro = (ate < esq + 7) ? esq + 7 : ((ate > dir - 7) ? dir - 7 : ate);
        render.retangulo(D2D1::RectF(centro - 7, trilhoY - 4, centro + 7, trilhoY + 9),
                         tema::kTexto, 7);
        y += 26;
    }

    // ---- quem está na sala
    std::vector<Participante> copia;
    {
        std::lock_guard trava(travaPares);
        copia = pares;
    }

    render.linha(esq, y, dir, y, tema::kLinha);
    y += 16;
    secao((L"NA SALA  (" + std::to_wstring(copia.size() + 1) + L")").c_str());

    // Uma pessoa por linha, com marca de quem está no palco.
    //
    // Isto responde à pergunta que a tela antiga deixava no ar: o palco troca
    // sozinho para quem transmite, e nada dizia que aquilo tinha acontecido nem
    // de quem era a tela. Agora o ponto verde diz.
    auto pessoa = [&](const std::wstring& nome, int ping, bool souEu, bool noPalco) {
        const auto area = D2D1::RectF(esq, y, dir, y + 26);
        if (noPalco) render.retangulo(area, tema::kVerdeSuave, 7);

        float x = area.left + (noPalco ? 10.0f : 2.0f);
        if (noPalco) {
            render.retangulo(D2D1::RectF(x, area.top + 11, x + 5, area.top + 16), tema::kVerde, 3);
            x += 12;
        }
        render.texto(nome, D2D1::RectF(x, area.top, area.right - 62, area.bottom),
                     souEu ? tema::kVerde : tema::kTexto, Fonte::Pequena);
        render.texto(std::to_wstring(ping) + L" ms",
                     D2D1::RectF(area.left, area.top, area.right - 8, area.bottom),
                     tema::kApagado, Fonte::Pequena, DWRITE_TEXT_ALIGNMENT_TRAILING);
        y += 28;
    };

    const std::wstring meuNome = campoNome.valor.empty() ? L"Você" : campoNome.valor + L"  (você)";
    pessoa(meuNome, sinal.pingMs(), true, transmitindo && !escolhida);

    for (const auto& p : copia) {
        const std::wstring nome = paraW(p.nome);
        const bool estaNoPalco = escolhida && escolhida->nome == nome;
        pessoa(nome, p.pingMs, false, estaNoPalco);
    }

    // ---- transmissões: um cartão com miniatura por pessoa transmitindo
    //
    // É o que faltava para ver a tela de quem está na chamada. Antes o palco
    // trocava sozinho e não havia como escolher nem saber quantas transmissões
    // existiam - com duas pessoas transmitindo, uma delas simplesmente não
    // aparecia em lugar nenhum.
    //
    // Cada cartão desenha a miniatura ao vivo daquela transmissão, e clicar põe
    // no palco. É o mesmo desenho da coluna de cartões do cliente em Electron.
    btTransmissoes.clear();
    idsTransmissoes.clear();

    if (!aoVivo.empty()) {
        y += 10;
        render.linha(esq, y, dir, y, tema::kLinha);
        y += 16;
        secao(aoVivo.size() > 1 ? (L"TRANSMITINDO  (" + std::to_wstring(aoVivo.size()) +
                                    L")  ·  CLIQUE PARA VER")
                                     .c_str()
                                 : (L"TRANSMITINDO  (" + std::to_wstring(aoVivo.size()) + L")").c_str());

        for (const auto& n : aoVivo) {
            // 16:9 na largura disponível, mais uma faixa para o nome.
            const float larguraCartao = dir - esq;
            const float alturaMini = larguraCartao * 9.0f / 16.0f;
            const auto cartao = D2D1::RectF(esq, y, dir, y + alturaMini + 26);

            const bool ativo = n.id == noPalco;
            render.retangulo(cartao, ativo ? tema::kVerdeSuave : tema::kPainel2, 10);

            const auto areaMini =
                D2D1::RectF(cartao.left + 4, cartao.top + 4, cartao.right - 4, cartao.top + alturaMini);
            render.retangulo(areaMini, tema::kFundo, 7);

            // Quem está no palco NÃO é desenhado de novo aqui.
            //
            // O renderizador guarda, por chave, onde a imagem caiu - e é disso
            // que a etiqueta em cima do vídeo se serve. Desenhando a mesma
            // chave duas vezes, a segunda (a miniatura) sobrescrevia a
            // primeira, e a etiqueta ia parar em cima do cartão em vez de em
            // cima do palco. Além disso, decodificar já é caro; desenhar a
            // mesma imagem duas vezes por quadro é gasto sem nada em troca.
            if (ativo) {
                render.texto(L"no palco", areaMini, tema::kVerde, Fonte::Pequena,
                             DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                render.video(n.id, n.quadro, areaMini);
            }

            render.texto(ativo ? (L"●  " + n.nome) : n.nome,
                         D2D1::RectF(cartao.left + 10, cartao.top + alturaMini,
                                     cartao.right - 10, cartao.bottom),
                         ativo ? tema::kVerde : tema::kTexto, Fonte::Pequena);

            btTransmissoes.push_back(cartao);
            idsTransmissoes.push_back(n.id);
            y += alturaMini + 32;
        }
    }

    // ---- pé do painel: botões e, abaixo deles, o diagnóstico
    //
    // Ancorados na base em vez de seguirem a lista. Com a sala cheia, o rodapé
    // calculado por posição fixa se sobrepunha aos nomes.
    const float alturaDiagnostico = transmitindo ? 74.0f : 0.0f;
    float base = painel.bottom - 16 - alturaDiagnostico;

    btSair = D2D1::RectF(esq, base - 38, esq + 84, base);
    desenharBotao(btSair, L"SAIR", false);
    btTransmitir = D2D1::RectF(esq + 92, base - 38, dir, base);
    desenharBotao(btTransmitir, transmitindo ? L"PARAR" : L"TRANSMITIR", transmitindo);

    if (!transmitindo) return;

    // Diagnóstico: três linhas, e só o que muda de verdade enquanto se
    // transmite. A versão anterior tinha oito linhas de rótulo e valor - era
    // mais alta que a lista de participantes e ninguém lia.
    float d = painel.bottom - 62;
    auto info = [&](const std::wstring& rotulo, const std::wstring& valor,
                    const D2D1_COLOR_F& cor) {
        render.texto(rotulo, D2D1::RectF(esq, d, dir, d + 16), tema::kApagado, Fonte::Pequena);
        render.texto(valor, D2D1::RectF(esq, d, dir, d + 16), cor, Fonte::Pequena,
                     DWRITE_TEXT_ALIGNMENT_TRAILING);
        d += 17;
    };

    render.linha(esq, painel.bottom - 74, dir, painel.bottom - 74, tema::kLinha);

    wchar_t buffer[64];
    ::swprintf_s(buffer, L"%.0f/s  ·  %s", fps,
                 encoder.porHardware() ? L"GPU" : L"CPU");
    info(L"captura", buffer, encoder.porHardware() ? tema::kVerde : tema::kApagado);

    size_t abertas = 0;
    std::wstring estadoMidia = L"sem ninguem";
    std::wstring caminhosMidia;
    {
        std::lock_guard trava(travaConexoes);
        for (auto& [id, c] : conexoes) {
            if (c->pronto()) ++abertas;
        }
        if (!conexoes.empty()) {
            ::swprintf_s(buffer, L"%zu/%zu", abertas, conexoes.size());
            estadoMidia = abertas > 0 ? buffer : paraW(conexoes.begin()->second->estado());
            caminhosMidia = paraW(conexoes.begin()->second->caminhos());
        }
    }
    info(L"conexao", estadoMidia, abertas > 0 ? tema::kVerde : tema::kApagado);

    // Sem caminho público nem retransmitido, quem está fora da sua rede não
    // recebe nada - e essa é a informação que faltava quando a conexão ficava
    // presa em "procurando caminho".
    if (!caminhosMidia.empty()) {
        const bool soLocal = caminhosMidia == L"local";
        info(L"caminho", caminhosMidia, soLocal ? tema::kVermelho : tema::kApagado);
    } else {
        info(L"audio", audio.pidExcluido() ? L"sem o Discord" : L"tudo",
             audio.ativo() ? tema::kVerde : tema::kApagado);
    }
}

void Aplicacao::Interno::desenhar() {
    render.comecarQuadro();
    render.limpar(tema::kFundo);

    if (telaAtual == Tela::Entrada) desenharEntrada();
    else desenharAoVivo();

    desenharBarraTitulo();
    render.terminarQuadro();
}

}  // namespace gl
