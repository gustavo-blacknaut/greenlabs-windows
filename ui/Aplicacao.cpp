#include "ui/Aplicacao.h"

#include <windows.h>

#include <windowsx.h>  // GET_X_LPARAM e GET_Y_LPARAM

#include <d3d11.h>
#include <objbase.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "capture/AudioCapture.h"
#include "capture/Cursor.h"
#include "config/Config.h"
#include "capture/ProcessTree.h"
#include "capture/ScreenCapture.h"
#include "encoder/VideoEncoder.h"
#include "network/Midia.h"
#include "network/Signaling.h"
#include "ui/Renderizador.h"
#include "ui/Tema.h"
#include "util/Log.h"
#include "video/ColorConverter.h"

using Microsoft::WRL::ComPtr;

namespace gl {
namespace {

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
    AudioCapture audio;
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
    bool transmitindo = false;
    std::wstring aviso;

    // O quadro mais recente da captura, para a prévia. A textura é da própria
    // duplicação e vale só até o próximo liberarQuadro().
    ID3D11Texture2D* quadroAtual = nullptr;

    std::mutex travaPares;
    std::vector<Participante> pares;

    // Uma conexão de mídia por participante. Numa sala de N pessoas são N-1:
    // é a topologia em malha, a mesma dos outros clientes.
    std::mutex travaConexoes;
    std::map<std::string, std::shared_ptr<ConexaoPar>> conexoes;
    std::string meuId;
    std::atomic<bool> conectado{false};

    // Botões, guardados entre o desenho e o clique.
    D2D1_RECT_F btMinimizar{}, btMaximizar{}, btFechar{};
    D2D1_RECT_F btEntrar{}, btTransmitir{}, btSair{};
    std::vector<D2D1_RECT_F> btMonitores;
    std::vector<D2D1_RECT_F> btQualidades;

    double fps = 0;
    int64_t quadrosNoSegundo = 0;
    std::chrono::steady_clock::time_point marcaFps = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point ultimoEncode = std::chrono::steady_clock::now();

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
    ouvintes.aoEntrar = [this](const std::string& eu, const std::vector<Participante>& pares) {
        {
            std::lock_guard trava(d_->travaPares);
            d_->meuId = eu;
            d_->pares = pares;
        }
        d_->conectado.store(true);
        d_->telaAtual = Tela::AoVivo;

        // Chegamos numa sala que já tinha gente: nós é que oferecemos.
        if (d_->transmitindo) {
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
        if (d_->transmitindo) {
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

int Aplicacao::rodar() {
    MSG msg{};
    for (;;) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                d_->pararTransmissao();
                d_->sinal.sair();
                return static_cast<int>(msg.wParam);
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }

        d_->bombearCaptura();
        d_->desenhar();
    }
}

// ---------------------------------------------------------------- captura

void Aplicacao::Interno::bombearCaptura() {
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
                cursorPronto = cursor.iniciar(tela.dispositivo(), tela.contexto(), m.largura,
                                              m.altura);
            }
            if (cursorPronto && quadro.formaMudou) cursor.definirForma(tela.formaDoCursor());

            quadroAtual = cursorPronto ? cursor.compor(quadro.textura, quadro.cursorVisivel,
                                                       quadro.cursorX, quadro.cursorY)
                                       : quadro.textura;

            if (transmitindo) {
                // Taxa fixa: a duplicação entrega quadro sempre que a tela muda,
                // o que num jogo passa fácil de 100 por segundo. Codificar todos
                // gastaria GPU e banda para nada — o alvo é o fps escolhido, e o
                // que passar disso é descartado aqui, antes de custar encode.
                const auto agora = std::chrono::steady_clock::now();
                const auto intervalo =
                    std::chrono::microseconds(1'000'000 / kQualidades[qualidadeEscolhida].fps);
                if (agora - ultimoEncode >= intervalo) {
                    ultimoEncode = agora;
                    if (auto* nv12 = conversor.converter(quadroAtual)) {
                        encoder.codificar(nv12, std::chrono::duration_cast<std::chrono::microseconds>(
                                                    agora.time_since_epoch()).count());
                    }
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

    const auto agora = std::chrono::steady_clock::now();
    const auto decorrido = std::chrono::duration<double>(agora - marcaFps).count();
    if (decorrido >= 1.0) {
        fps = quadrosNoSegundo / decorrido;
        quadrosNoSegundo = 0;
        marcaFps = agora;
    }
}

bool Aplicacao::Interno::comecarTransmissao() {
    const auto& m = tela.monitor();
    const Qualidade& q = kQualidades[qualidadeEscolhida];

    // O Video Processor redimensiona de graça no mesmo passo da conversão de
    // cor, então a tela vai para o encoder já no tamanho escolhido: menos
    // pixels para codificar e menos banda, sem custo extra de CPU.
    if (!conversor.iniciar(tela.dispositivo(), tela.contexto(), m.largura, m.altura, q.largura,
                           q.altura)) {
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
    audio.iniciar(excluir, [](const float*, uint32_t) {});

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
    if (!transmitindo) return;
    transmitindo = false;
    encoder.parar();
    audio.parar();
    {
        std::lock_guard trava(travaConexoes);
        conexoes.clear();
    }
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
    config.monitor = monitorEscolhido;
    config.lembrarServidor(servidor);
    config.salvar();

    if (!sinal.entrar(servidor, paraUtf8(campoSala.valor), paraUtf8(campoNome.valor))) {
        aviso = L"Não foi possível conectar. Confira o endereço e se o servidor está no ar.";
        return;
    }
    aviso.clear();
}

// ---------------------------------------------------------------- midia

std::shared_ptr<ConexaoPar> Aplicacao::Interno::abrirMidiaPara(const std::string& peerId) {
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
        if (de < meuId) {
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

void Aplicacao::Interno::enviarQuadroParaTodos(const PacoteCodificado& pacote) {
    // Um encoder, N transportes. Codificar por destinatario multiplicaria o
    // custo pelo tamanho da sala e derrubaria a maquina numa chamada grande.
    std::lock_guard trava(travaConexoes);
    for (auto& [id, conexao] : conexoes) {
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
                pararTransmissao();

                // O renderizador vive no dispositivo D3D11 da captura. Trocar de
                // monitor destrói esse dispositivo, então ele precisa soltar
                // tudo ANTES - senão fica com uma cadeia de troca apontando para
                // um dispositivo morto, e a nova criação falha em silêncio
                // porque o DXGI só aceita uma cadeia por janela.
                render.liberar();
                cursorPronto = false;

                monitorEscolhido = novo;
                if (!tela.iniciar(static_cast<uint32_t>(novo))) {
                    aviso = L"Não foi possível capturar esse monitor.";
                    tela.iniciar(0);
                    monitorEscolhido = 0;
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

    for (size_t i = 0; i < btQualidades.size(); ++i) {
        if (dentro(btQualidades[i], x, y)) {
            const int novo = static_cast<int>(i);
            if (novo != qualidadeEscolhida) {
                const bool estava = transmitindo;
                pararTransmissao();
                qualidadeEscolhida = novo;
                // Trocar de qualidade refaz encoder e conversor, então a
                // transmissão precisa recomeçar - e recomeça sozinha para a
                // pessoa não achar que caiu.
                if (estava) comecarTransmissao();
            }
            return;
        }
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
    render.texto(L"v0.0.1  nativo", D2D1::RectF(124, 0, 280, tema::kAlturaTitulo), tema::kApagado,
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

void Aplicacao::Interno::desenharAoVivo() {
    const float larg = render.largura();
    const float alt = render.altura();
    const float topo = tema::kAlturaTitulo + tema::kEspaco;
    const float painelX = larg - tema::kLarguraPainelLateral - tema::kEspaco;

    // ---- palco
    const auto palco = D2D1::RectF(tema::kEspaco, topo, painelX - tema::kEspaco,
                                   alt - tema::kEspaco - 64);
    render.retangulo(palco, tema::kPainel, tema::kRaioCartao);

    // Passar quadroAtual mesmo nulo é de propósito: sem quadro novo o
    // renderizador repinta o último. Antes a prévia apagava a cada instante em
    // que a tela não mudava, e o resultado era piscar sem parar.
    render.video(quadroAtual, D2D1::RectF(palco.left + 6, palco.top + 6, palco.right - 6,
                                          palco.bottom - 6));
    if (!render.temQuadro()) {
        render.texto(L"Aguardando o primeiro quadro…", palco, tema::kApagado, Fonte::Subtitulo,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    render.contorno(palco, tema::kLinha, tema::kRaioCartao);

    // Selo de ao vivo por cima do vídeo, como no cliente em Electron.
    if (transmitindo) {
        const auto selo = D2D1::RectF(palco.left + 18, palco.top + 18, palco.left + 132,
                                      palco.top + 50);
        render.retangulo(selo, tema::kVerdeSuave, 10);
        render.contorno(selo, tema::kVerdeLinha, 10);
        render.texto(L"●  AO VIVO", selo, tema::kVerde, Fonte::Pequena,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // ---- barra inferior
    const float barraY = alt - tema::kEspaco - 52;
    btTransmitir = D2D1::RectF(tema::kEspaco, barraY, tema::kEspaco + 210, barraY + 44);
    desenharBotao(btTransmitir, transmitindo ? L"PARAR DE TRANSMITIR" : L"TRANSMITIR TELA",
                  transmitindo);

    btSair = D2D1::RectF(painelX - tema::kEspaco - 120, barraY, painelX - tema::kEspaco,
                         barraY + 44);
    desenharBotao(btSair, L"SAIR", false);

    // ---- painel lateral
    const auto painel = D2D1::RectF(painelX, topo, larg - tema::kEspaco, alt - tema::kEspaco);
    render.retangulo(painel, tema::kPainel, tema::kRaioCartao);
    render.contorno(painel, tema::kLinha, tema::kRaioCartao);

    float linhaY = painel.top + 20;
    render.texto(L"MONITOR", D2D1::RectF(painel.left + 20, linhaY, painel.right - 20, linhaY + 18),
                 tema::kApagado, Fonte::Pequena);
    linhaY += 26;

    btMonitores.clear();
    for (size_t i = 0; i < monitores.size(); ++i) {
        const auto area = D2D1::RectF(painel.left + 20, linhaY, painel.right - 20, linhaY + 40);
        btMonitores.push_back(area);

        const bool ativo = static_cast<int>(i) == monitorEscolhido;
        render.retangulo(area, ativo ? tema::kVerdeSuave : tema::kPainel2, tema::kRaioBotao);
        render.contorno(area, ativo ? tema::kVerdeLinha : tema::kLinha, tema::kRaioBotao);

        const std::wstring rotulo = paraW(monitores[i].nome) + L"   " +
                                    std::to_wstring(monitores[i].largura) + L"×" +
                                    std::to_wstring(monitores[i].altura);
        render.texto(rotulo, D2D1::RectF(area.left + 14, area.top, area.right - 14, area.bottom),
                     ativo ? tema::kVerde : tema::kTexto, Fonte::Pequena);
        linhaY += 48;
    }

    linhaY += 4;
    render.texto(L"QUALIDADE", D2D1::RectF(painel.left + 20, linhaY, painel.right - 20, linhaY + 18),
                 tema::kApagado, Fonte::Pequena);
    linhaY += 26;

    btQualidades.clear();
    for (size_t i = 0; i < std::size(kQualidades); ++i) {
        const auto area = D2D1::RectF(painel.left + 20, linhaY, painel.right - 20, linhaY + 36);
        btQualidades.push_back(area);

        const bool ativo = static_cast<int>(i) == qualidadeEscolhida;
        render.retangulo(area, ativo ? tema::kVerdeSuave : tema::kPainel2, tema::kRaioBotao);
        render.contorno(area, ativo ? tema::kVerdeLinha : tema::kLinha, tema::kRaioBotao);
        render.texto(kQualidades[i].rotulo,
                     D2D1::RectF(area.left + 14, area.top, area.right - 60, area.bottom),
                     ativo ? tema::kVerde : tema::kTexto, Fonte::Pequena);
        render.texto(std::to_wstring(kQualidades[i].bitrate / 1000) + L" kbps",
                     D2D1::RectF(area.left, area.top, area.right - 14, area.bottom),
                     tema::kApagado, Fonte::Pequena, DWRITE_TEXT_ALIGNMENT_TRAILING);
        linhaY += 42;
    }

    linhaY += 6;
    render.linha(painel.left + 20, linhaY, painel.right - 20, linhaY, tema::kLinha);
    linhaY += 20;

    std::vector<Participante> copia;
    std::string eu;
    {
        std::lock_guard trava(travaPares);
        copia = pares;
        eu = meuId;
    }

    render.texto(L"NA SALA  (" + std::to_wstring(copia.size() + 1) + L")",
                 D2D1::RectF(painel.left + 20, linhaY, painel.right - 20, linhaY + 18),
                 tema::kApagado, Fonte::Pequena);
    linhaY += 28;

    const std::wstring meuNome =
        campoNome.valor.empty() ? L"Você" : campoNome.valor + L"  (você)";
    render.texto(meuNome, D2D1::RectF(painel.left + 20, linhaY, painel.right - 80, linhaY + 22),
                 tema::kVerde, Fonte::Corpo);
    render.texto(std::to_wstring(sinal.pingMs()) + L" ms",
                 D2D1::RectF(painel.right - 90, linhaY, painel.right - 20, linhaY + 22),
                 tema::kApagado, Fonte::Pequena, DWRITE_TEXT_ALIGNMENT_TRAILING);
    linhaY += 28;

    for (const auto& p : copia) {
        render.texto(paraW(p.nome),
                     D2D1::RectF(painel.left + 20, linhaY, painel.right - 80, linhaY + 22),
                     tema::kTexto, Fonte::Corpo);
        render.texto(std::to_wstring(p.pingMs) + L" ms",
                     D2D1::RectF(painel.right - 90, linhaY, painel.right - 20, linhaY + 22),
                     tema::kApagado, Fonte::Pequena, DWRITE_TEXT_ALIGNMENT_TRAILING);
        linhaY += 28;
    }

    // ---- rodapé do painel: o que está realmente acontecendo
    float rodape = painel.bottom - 118;
    render.linha(painel.left + 20, rodape - 12, painel.right - 20, rodape - 12, tema::kLinha);

    auto linhaInfo = [&](const std::wstring& rotulo, const std::wstring& valor,
                         const D2D1_COLOR_F& cor) {
        render.texto(rotulo, D2D1::RectF(painel.left + 20, rodape, painel.right - 20, rodape + 20),
                     tema::kApagado, Fonte::Pequena);
        render.texto(valor, D2D1::RectF(painel.left + 20, rodape, painel.right - 20, rodape + 20),
                     cor, Fonte::Pequena, DWRITE_TEXT_ALIGNMENT_TRAILING);
        rodape += 22;
    };

    wchar_t buffer[32];
    ::swprintf_s(buffer, L"%.0f/s", fps);
    linhaInfo(L"captura", buffer, tema::kTexto);

    if (transmitindo) {
        linhaInfo(L"encoder", paraW(encoder.nomeDoEncoder()).substr(0, 18),
                  encoder.porHardware() ? tema::kVerde : tema::kApagado);
        ::swprintf_s(buffer, L"%llu", static_cast<unsigned long long>(encoder.quadrosCodificados()));
        linhaInfo(L"codificados", buffer, tema::kTexto);
        linhaInfo(L"audio",
                  audio.pidExcluido() ? L"sem o Discord" : L"tudo",
                  audio.ativo() ? tema::kVerde : tema::kApagado);
        size_t abertas = 0;
        uint64_t pacotes = 0;
        std::wstring estadoMidia = L"sem ninguem";
        std::wstring caminhosMidia;
        {
            std::lock_guard trava(travaConexoes);
            for (auto& [id, c] : conexoes) {
                if (c->pronto()) ++abertas;
                pacotes += c->pacotesEnviados();
            }
            if (!conexoes.empty()) {
                // Estado por extenso quando nada conectou: "0/1" nao diz se esta
                // negociando, se falhou ou se nem comecou.
                ::swprintf_s(buffer, L"%zu/%zu", abertas, conexoes.size());
                estadoMidia = abertas > 0 ? buffer
                                          : paraW(conexoes.begin()->second->estado());
                caminhosMidia = paraW(conexoes.begin()->second->caminhos());
            }
        }
        linhaInfo(L"midia", estadoMidia, abertas > 0 ? tema::kVerde : tema::kApagado);
        if (!caminhosMidia.empty()) {
            // Sem caminho público nem retransmitido, quem está fora da sua rede
            // não recebe nada - e essa é a informação que faltava quando a
            // conexão ficava presa "procurando caminho".
            const bool soLocal = caminhosMidia == L"local";
            linhaInfo(L"caminho", caminhosMidia, soLocal ? tema::kVermelho : tema::kTexto);
        }
        ::swprintf_s(buffer, L"%llu", static_cast<unsigned long long>(pacotes));
        linhaInfo(L"quadros enviados", buffer, tema::kTexto);
    } else {
        linhaInfo(L"encoder", L"parado", tema::kApagado);
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
