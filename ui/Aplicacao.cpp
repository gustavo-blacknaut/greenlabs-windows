#include "ui/Aplicacao.h"

#include <windows.h>

#include <windowsx.h>  // GET_X_LPARAM e GET_Y_LPARAM

#include <d3d11.h>
#include <objbase.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "capture/AudioCapture.h"
#include "capture/ProcessTree.h"
#include "capture/ScreenCapture.h"
#include "encoder/VideoEncoder.h"
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

// Campo de texto: guarda o conteúdo e se está com o foco. O desenho fica na
// Aplicacao, que é quem conhece o layout.
struct Campo {
    std::wstring valor;
    std::wstring dica;
    D2D1_RECT_F area{};
    bool focado = false;
};

}  // namespace

struct Aplicacao::Interno {
    HWND janela = nullptr;
    Renderizador render;

    ScreenCapture tela;
    ColorConverter conversor;
    VideoEncoder encoder;
    AudioCapture audio;
    Signaling sinal;

    Tela telaAtual = Tela::Entrada;

    Campo campoNome{L"", L"como os outros vao te ver"};
    Campo campoServidor{L"", L"exemplo.com:25640"};
    Campo campoSala{L"call1", L"call1"};

    std::vector<MonitorInfo> monitores;
    int monitorEscolhido = 0;
    bool transmitindo = false;
    std::wstring aviso;

    // O quadro mais recente da captura, para a prévia. A textura é da própria
    // duplicação e vale só até o próximo liberarQuadro().
    ID3D11Texture2D* quadroAtual = nullptr;

    std::mutex travaPares;
    std::vector<Participante> pares;
    std::string meuId;
    std::atomic<bool> conectado{false};

    // Botões, guardados entre o desenho e o clique.
    D2D1_RECT_F btMinimizar{}, btMaximizar{}, btFechar{};
    D2D1_RECT_F btEntrar{}, btTransmitir{}, btSair{};
    std::vector<D2D1_RECT_F> btMonitores;

    double fps = 0;
    int64_t quadrosNoSegundo = 0;
    std::chrono::steady_clock::time_point marcaFps = std::chrono::steady_clock::now();

    void desenhar();
    void desenharBarraTitulo();
    void desenharEntrada();
    void desenharAoVivo();
    void clique(float x, float y);
    void tecla(wchar_t c);
    void bombearCaptura();
    bool comecarTransmissao();
    void pararTransmissao();
    void conectar();

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

bool Aplicacao::iniciar(const std::wstring& titulo, int largura, int altura,
                        const Inicial& inicial) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW classe{};
    classe.cbSize = sizeof(classe);
    classe.lpfnWndProc = procedimento;
    classe.hInstance = ::GetModuleHandleW(nullptr);
    classe.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
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
        return false;
    }
    ::SetWindowLongPtrW(d_->janela, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d_.get()));

    d_->monitores = ScreenCapture::listarMonitores();
    if (d_->monitores.empty()) {
        erro("nenhum monitor encontrado");
        return false;
    }
    if (!d_->tela.iniciar(0)) return false;
    if (!d_->render.iniciar(d_->janela, d_->tela.dispositivo())) return false;

    Signaling::Ouvintes ouvintes;
    ouvintes.aoEntrar = [this](const std::string& eu, const std::vector<Participante>& pares) {
        std::lock_guard trava(d_->travaPares);
        d_->meuId = eu;
        d_->pares = pares;
        d_->conectado.store(true);
        d_->telaAtual = Tela::AoVivo;
    };
    ouvintes.aoChegarAlguem = [this](const Participante& p) {
        std::lock_guard trava(d_->travaPares);
        d_->pares.push_back(p);
    };
    ouvintes.aoSairAlguem = [this](const std::string& id) {
        std::lock_guard trava(d_->travaPares);
        std::erase_if(d_->pares, [&](const Participante& p) { return p.id == id; });
    };
    ouvintes.aoCair = [this](const std::string& motivo) {
        d_->conectado.store(false);
        d_->aviso = L"A conexao caiu: " + paraW(motivo);
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
            quadroAtual = quadro.textura;
            quadrosNoSegundo += 1;

            if (transmitindo) {
                if (auto* nv12 = conversor.converter(quadro.textura)) {
                    const auto agora = std::chrono::steady_clock::now().time_since_epoch();
                    encoder.codificar(
                        nv12, std::chrono::duration_cast<std::chrono::microseconds>(agora).count());
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
    if (!conversor.iniciar(tela.dispositivo(), tela.contexto(), m.largura, m.altura, m.largura,
                           m.altura)) {
        aviso = L"Nao foi possivel preparar a conversao de cor.";
        return false;
    }

    ConfigEncoder cfg;
    cfg.largura = conversor.largura();
    cfg.altura = conversor.altura();
    cfg.fps = 60;
    cfg.bitrate = 4'500'000;

    // O consumidor ainda só conta: o transporte de mídia é a etapa seguinte.
    // Quando ela existir, é aqui que o pacote vai para a rede.
    if (!encoder.iniciar(tela.dispositivo(), cfg, [](const PacoteCodificado&) {})) {
        aviso = L"Nao foi possivel iniciar o encoder H.264.";
        return false;
    }
    encoder.pedirQuadroChave();

    // Áudio: tudo menos o Discord.
    const uint32_t excluir = acharRaizParaExcluir(
        {"discord", "discordptb", "discordcanary", "discorddevelopment"});
    audio.iniciar(excluir, [](const float*, uint32_t) {});

    transmitindo = true;
    aviso.clear();
    return true;
}

void Aplicacao::Interno::pararTransmissao() {
    if (!transmitindo) return;
    transmitindo = false;
    encoder.parar();
    audio.parar();
}

void Aplicacao::Interno::conectar() {
    const std::string servidor = paraUtf8(campoServidor.valor);
    if (servidor.empty()) {
        aviso = L"Informe o endereco do servidor.";
        return;
    }
    aviso = L"Conectando...";
    if (!sinal.entrar(servidor, paraUtf8(campoSala.valor), paraUtf8(campoNome.valor))) {
        aviso = L"Nao foi possivel conectar. Confira o endereco e se o servidor esta no ar.";
        return;
    }
    aviso.clear();
}

// ---------------------------------------------------------------- entrada

Campo* Aplicacao::Interno::campoFocado() {
    if (campoNome.focado) return &campoNome;
    if (campoServidor.focado) return &campoServidor;
    if (campoSala.focado) return &campoSala;
    return nullptr;
}

void Aplicacao::Interno::tecla(wchar_t c) {
    Campo* campo = campoFocado();
    if (!campo) return;
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
        campoNome.focado = dentro(campoNome.area, x, y);
        campoServidor.focado = dentro(campoServidor.area, x, y);
        campoSala.focado = dentro(campoSala.area, x, y);
        if (dentro(btEntrar, x, y)) conectar();
        return;
    }

    for (size_t i = 0; i < btMonitores.size(); ++i) {
        if (dentro(btMonitores[i], x, y)) {
            const int novo = static_cast<int>(i);
            if (novo != monitorEscolhido) {
                pararTransmissao();
                monitorEscolhido = novo;
                tela.iniciar(static_cast<uint32_t>(novo));
                // O renderizador vive no dispositivo D3D11 da captura, e trocar
                // de monitor pode trocar de placa.
                render.iniciar(janela, tela.dispositivo());
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
        const bool piscar = campo.focado &&
                            (::GetTickCount64() / 500) % 2 == 0;
        render.texto(campo.valor + (piscar ? L"|" : L""), interna, tema::kTexto, Fonte::Corpo);
    }
}

void Aplicacao::Interno::desenharBarraTitulo() {
    const float larg = render.largura();
    render.retangulo(D2D1::RectF(0, 0, larg, tema::kAlturaTitulo), tema::kPainel);
    render.linha(0, tema::kAlturaTitulo, larg, tema::kAlturaTitulo, tema::kLinha);

    render.texto(L"GreenLabs", D2D1::RectF(16, 0, 200, tema::kAlturaTitulo), tema::kTexto,
                 Fonte::Botao);
    render.texto(L"v0.0.1  nativo", D2D1::RectF(96, 0, 260, tema::kAlturaTitulo), tema::kApagado,
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

    render.texto(L"SEM CONTA · SEM LIMITE DE TEMPO",
                 D2D1::RectF(x + 36, y + 34, x + largCartao - 36, y + 54), tema::kVerde,
                 Fonte::Pequena);
    render.texto(L"Entrar numa sala",
                 D2D1::RectF(x + 36, y + 58, x + largCartao - 36, y + 100), tema::kTexto,
                 Fonte::Titulo);

    const float larguraCampo = largCartao - 72;
    campoNome.area = D2D1::RectF(x + 36, y + 140, x + 36 + larguraCampo, y + 186);
    campoServidor.area = D2D1::RectF(x + 36, y + 222, x + 36 + larguraCampo, y + 268);
    campoSala.area = D2D1::RectF(x + 36, y + 304, x + 36 + larguraCampo, y + 350);

    desenharCampo(campoNome, L"SEU APELIDO");
    desenharCampo(campoServidor, L"SERVIDOR");
    desenharCampo(campoSala, L"SALA");

    btEntrar = D2D1::RectF(x + 36, y + 380, x + 36 + larguraCampo, y + 428);
    desenharBotao(btEntrar, L"ENTRAR NA SALA", true);

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

    if (quadroAtual) {
        render.video(quadroAtual, D2D1::RectF(palco.left + 6, palco.top + 6, palco.right - 6,
                                              palco.bottom - 6));
    } else {
        render.texto(L"Aguardando a tela mudar…", palco, tema::kApagado, Fonte::Subtitulo,
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

    linhaY += 10;
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
        campoNome.valor.empty() ? L"Voce" : campoNome.valor + L"  (voce)";
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
