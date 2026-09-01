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
#include "capture/CameraCapture.h"
#include "capture/Cursor.h"
#include "config/Config.h"
#include "capture/ProcessTree.h"
#include "capture/ScreenCapture.h"
#include "decoder/VideoDecoder.h"
#include "encoder/VideoEncoder.h"
#include "network/Midia.h"
#include "network/Signaling.h"
#include "ui/Icones.h"
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
        // Quem transmite. Vem do identificador da faixa; vazio enquanto o dono
        // ainda nao apareceu na lista de participantes.
        std::string dono;
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

    /// Solta a câmera antes de o dispositivo D3D morrer.
    ///
    /// As texturas dela pertencem ao dispositivo da captura, e trocar de
    /// monitor cria um novo. Deixar a câmera viva por cima disso é ficar com
    /// ponteiro para memória de uma GPU que já não é a nossa.
    void soltarCamerasDoDispositivo() {
        for (auto& c : camerasAbertas) {
            c->captura.parar();
            render.esquecerVideo("cam:" + c->id);
        }
        camerasAbertas.clear();
    }

    /// Reabre a câmera escolhida no dispositivo atual, se houver uma.
    void reabrirCameras() {
        const auto escolhidas = camerasEscolhidas;
        soltarCamerasDoDispositivo();
        aplicarCameras(escolhidas);
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

    // O monitor que a captura principal abre. É dele que sai o dispositivo
    // D3D11 usado por todo o resto - encoder, decodificador e interface.
    int monitorEscolhido = 0;

    // Telas EXTRAS, que entram lado a lado com a principal.
    //
    // Cada uma abre a duplicação no MESMO dispositivo da principal: o Video
    // Processor compõe as duas numa passada, e ele não atravessa dispositivos.
    // Por isso elas usam iniciarCom, e por isso um monitor ligado em outra
    // placa não pode entrar - o que acontece em notebook híbrido.
    std::vector<std::unique_ptr<ScreenCapture>> telasExtras;
    // As telas que vão na transmissão, por índice de monitor.
    //
    // Pode ser vazia: nesse caso vão só as câmeras, e "só a câmera" deixa de
    // ser um modo com botão próprio para ser o que ele sempre foi - nenhuma
    // tela marcada. Um modo a menos para explicar, e um a menos para manter.
    std::vector<int> telasLigadas;

    // As câmeras que vão na transmissão, pelo caminho do dispositivo.
    std::vector<std::string> camerasEscolhidas;

    bool telaLigada(int indice) const {
        return std::find(telasLigadas.begin(), telasLigadas.end(), indice) != telasLigadas.end();
    }
    bool cameraLigada(const std::string& id) const {
        return std::find(camerasEscolhidas.begin(), camerasEscolhidas.end(), id) !=
               camerasEscolhidas.end();
    }

    /// Abre a duplicação das telas ligadas que não são a principal.
    ///
    /// A principal já é capturada pelo `tela`, que é também de onde sai o
    /// dispositivo D3D de todo o resto. As outras entram por iniciarCom, no
    /// mesmo dispositivo: o Video Processor compõe numa passada, e ele não
    /// atravessa dispositivos.
    void abrirTelasExtras() {
        fecharTelasExtras();
        for (int indice : telasLigadas) {
            if (indice == monitorEscolhido) continue;
            auto captura = std::make_unique<ScreenCapture>();
            if (captura->iniciarCom(tela.dispositivo(), tela.contexto(),
                                    static_cast<uint32_t>(indice))) {
                telasExtras.push_back(std::move(captura));
            }
        }
    }

    void fecharTelasExtras() {
        for (auto& extra : telasExtras) extra->parar();
        telasExtras.clear();
        for (auto& copia : copiasExtras) copia.Reset();
        copiasExtras.clear();
    }

    // Cópia própria de cada tela extra.
    //
    // A textura que a duplicação entrega vale só até a próxima leitura, e a
    // duplicação só entrega quando aquela tela muda. Sem a cópia, um monitor
    // parado deixaria de existir no quadro seguinte - piscando entre a imagem e
    // preto conforme a outra tela se mexesse.
    std::vector<ComPtr<ID3D11Texture2D>> copiasExtras;

    bool montarEncoder(uint32_t largura, uint32_t altura, const Qualidade& q);
    void trocarPrincipal(int novo);
    int qualidadeEscolhida = 1;  // 1080p 30fps

    // Mandar o som do sistema junto com a tela. Só vale para o que sai daqui:
    // o que chega dos outros toca sempre.
    bool audioLigado = true;

    // Câmera.
    //
    // Ela NÃO vai numa segunda faixa de vídeo: o servidor abre um transceiver
    // de vídeo por pessoa, e o id da faixa de saída é montado a partir do dono
    // mais o tipo - duas faixas de vídeo do mesmo dono colidiriam e a segunda
    // seria descartada em silêncio. A câmera entra COMPOSTA no mesmo quadro,
    // pelo Video Processor, e por isso quem assiste pelo Electron ou pelo
    // celular a vê sem que nada mude do lado deles.
    // Uma câmera aberta.
    //
    // São várias porque a pessoa pode ligar todas as que tem - a webcam da
    // mesa e a do celular por Iriun, por exemplo, e as duas entram no quadro.
    // Cada uma traz o próprio conversor para a tela: a câmera entrega NV12 e o
    // desenho quer BGRA, e o tamanho de uma não serve para a outra.
    struct CameraAberta {
        std::string id;
        std::string nome;
        CameraCapture captura;

        ColorConverter paraTela;
        bool paraTelaPronta = false;
        uint32_t paraTelaL = 0;
        uint32_t paraTelaA = 0;

        /// O quadro mais recente já em BGRA, pronto para desenhar. Nulo
        /// enquanto a câmera não entregar nada.
        ID3D11Texture2D* previa(ID3D11Device* dispositivo, ID3D11DeviceContext* contexto) {
            ID3D11Texture2D* nv12 = captura.quadro();
            if (!nv12) return nullptr;
            const uint32_t l = captura.largura();
            const uint32_t a = captura.altura();
            if (l == 0 || a == 0) return nullptr;
            if (!paraTelaPronta || paraTelaL != l || paraTelaA != a) {
                paraTelaPronta =
                    paraTela.iniciar(dispositivo, contexto, l, a, l, a, ColorConverter::Saida::Bgra);
                paraTelaL = l;
                paraTelaA = a;
                if (!paraTelaPronta) return nullptr;
            }
            return paraTela.converter(nv12);
        }
    };

    // As que estão abertas agora, na ordem em que foram escolhidas.
    std::vector<std::unique_ptr<CameraAberta>> camerasAbertas;

    // O que o computador tem, lido no arranque. Enumerar não acende luzinha.
    std::vector<CameraInfo> cameras;

    /// Acha uma câmera aberta pelo caminho do dispositivo.
    CameraAberta* cameraAberta(const std::string& id) {
        for (auto& c : camerasAbertas) {
            if (c->id == id) return c.get();
        }
        return nullptr;
    }

    /// Quantas estão abertas e entregando imagem.
    size_t camerasVivas() const {
        size_t total = 0;
        for (const auto& c : camerasAbertas) {
            if (c->captura.ativa()) ++total;
        }
        return total;
    }

    /// Abre e fecha câmeras até a lista de abertas bater com a escolhida.
    ///
    /// Chamada depois de qualquer mudança na escolha. Não derruba a conexão:
    /// só o encoder e o conversor são refeitos, pelo mesmo caminho da troca de
    /// qualidade - derrubar a chamada inteira para acender uma webcam faria
    /// todo mundo na sala perder a imagem por dois segundos.
    void aplicarCameras(const std::vector<std::string>& escolhidas);

    // Prévia da câmera na interface.

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

    // Onde o ponteiro está.
    //
    // O desenho aqui é imediato: nada guarda estado de "este botão está sob o
    // cursor". Guardar a posição do último movimento é o que permite a um botão
    // se acender ao ser apontado - e sem isso a interface inteira parecia
    // morta, porque nada respondia antes do clique.
    float mouseX = -1;
    float mouseY = -1;

    // Recalculado a cada desenho, para o ponteiro virar mãozinha em cima do que
    // é clicável. Sem isto não havia como saber o que era botão e o que era
    // enfeite.
    bool sobreClicavel = false;

    bool sobre(const D2D1_RECT_F& area) const {
        return mouseX >= area.left && mouseX <= area.right && mouseY >= area.top &&
               mouseY <= area.bottom;
    }

    /// Marca a área como clicável para o quadro atual e diz se o ponteiro está
    /// nela. Usar isto em vez de sobre() é o que mantém o cursor certo.
    bool apontando(const D2D1_RECT_F& area) {
        if (!sobre(area)) return false;
        sobreClicavel = true;
        return true;
    }

    // Rolagem do painel lateral.
    //
    // O painel empilhava monitores, qualidades, câmeras, som, volume, a sala
    // inteira e um cartão por transmissão numa coluna de altura fixa. Com
    // quatro pessoas na sala o fim da lista passava por baixo dos botões, e
    // clicar num nome acertava o SAIR. Agora a lista rola e o que sai da área
    // é recortado.
    float rolagem = 0;
    float alturaConteudo = 0;
    float alturaVisivel = 0;
    D2D1_RECT_F areaRolavel{};

    void rolar(float passos) {
        const float limite = alturaConteudo - alturaVisivel;
        if (limite <= 0) { rolagem = 0; return; }
        rolagem -= passos * 48.0f;
        if (rolagem < 0) rolagem = 0;
        if (rolagem > limite) rolagem = limite;
    }

    // Tamanho para o qual o conversor, o encoder e o cursor foram montados.
    // Zero significa "ainda não montei nada", e aí a primeira captura não conta
    // como mudança.
    uint32_t larguraMontada = 0;
    uint32_t alturaMontada = 0;
    bool precisaRemontar = false;

    /// Refaz o caminho de vídeo depois de o monitor mudar de resolução.
    ///
    /// Roda no laço principal, fora da trava da captura: pararEncodeSomente
    /// pega essa mesma trava, e chamar daqui de dentro travaria o programa.
    void remontarVideo() {
        precisaRemontar = false;
        // As medidas guardadas na lista são as antigas, e é delas que sai o
        // tamanho de saída do encoder.
        monitores = ScreenCapture::listarMonitores();
        reiniciarEncode();
    }

    // ---- modal "Escolha o que transmitir"
    //
    // É a mesma janela do cliente em Electron: um cartão no meio da tela, com
    // abas para telas e câmeras, miniaturas ao vivo e o interruptor do som no
    // pé. Escolher o que vai no ar deixou de ser uma lista solta na lateral e
    // virou um momento - que é o que a pessoa espera ao clicar em TRANSMITIR.
    //
    // A escolha fica AQUI enquanto o modal está aberto, e só vale ao confirmar.
    // Aplicar a cada clique acenderia e apagaria webcam a cada indecisão.
    enum class Modal { Nenhum, Fonte, Config };
    Modal modal = Modal::Nenhum;
    bool modalAberto() const { return modal != Modal::Nenhum; }

    int abaModal = 0;  // 0 telas, 1 câmeras
    std::vector<int> telasNoModal;
    std::vector<std::string> camerasNoModal;
    bool audioNoModal = true;
    int qualidadeNoModal = 1;

    D2D1_RECT_F btModalFechar{}, btModalConfirmar{}, btModalAudio{};
    D2D1_RECT_F btAbaTelas{}, btAbaCameras{};
    std::vector<D2D1_RECT_F> btCartoesModal;

    // Modal de configuracao.
    int abaConfig = 0;  // 0 conexao, 1 servidores
    std::wstring avisoConfig;
    float alturaConteudoConfig = 0;
    std::vector<D2D1_RECT_F> btAbasConfig;
    std::vector<D2D1_RECT_F> btServidoresConfig;
    std::vector<D2D1_RECT_F> btRemoverServidor;
    D2D1_RECT_F btConfigConcluir{}, btSalvarPadrao{}, btRestaurar{}, btEngrenagem{};

    // Barra de acoes: em quantos quadros o palco se divide, e os botoes dela.
    //
    // 1, 2 ou 4, como no Electron. Com mais quadros do que transmissoes, os que
    // sobram ficam vazios de proposito: sem eles, a unica transmissao pularia de
    // tamanho a cada pessoa que entra.
    int divisoes = 1;
    D2D1_RECT_F btBarraFonte{}, btBarraCamera{}, btBarraSair{}, btBarraTransmitir{};
    std::vector<D2D1_RECT_F> btDivisoes;
    void desenharBarraDeAcoes();

    // Os quadros do palco e o que ha em cada um, para o clique saber onde caiu.
    std::vector<D2D1_RECT_F> btQuadros;
    std::vector<std::string> chavesQuadros;
    D2D1_RECT_F btExpandirQuadro{};

    // Campo desenhado sem o rotulo por cima - dentro do modal o rotulo ja vem
    // separado, e o desenharCampo original o desenha acima da area.
    void desenharCampoSimples(Campo& campo);

    // Rolagem da grade do modal, medida como a do painel lateral.
    float rolagemModal = 0;
    float alturaConteudoModal = 0;
    D2D1_RECT_F areaRolavelModal{};

    // As duplicações abertas só para o modal mostrar miniatura de cada monitor.
    // Fecham quando ele fecha: manter duplicação de tudo aberta o tempo todo
    // custaria uma cópia por monitor por quadro, à toa.
    std::vector<std::unique_ptr<ScreenCapture>> previasDoModal;
    std::vector<ComPtr<ID3D11Texture2D>> quadrosDoModal;

    void abrirModal();
    void abrirConfig();
    void fecharModal();

    // Moldura comum aos dois modais: fundo escuro, cartao, cabecalho e abas.
    // Devolve o retangulo do corpo. O que muda de um para o outro e so o que
    // vai dentro - e por isso os dois sao identicos por construcao, e nao por
    // eu ter copiado as medidas de um para o outro e lembrado de manter.
    struct MolduraModal {
        D2D1_RECT_F cartao;
        D2D1_RECT_F corpo;
        float basePe;
    };
    MolduraModal desenharMoldura(int qualIcone, const std::wstring& titulo,
                                 const std::wstring& subtitulo, float alturaPe,
                                 const std::vector<std::wstring>& abas, int abaAtiva,
                                 std::vector<D2D1_RECT_F>& areasDasAbas);
    void desenharConfig();
    void confirmarModal();
    void desenharModal();
    void bombearPreviasDoModal();

    // Tela cheia: o palco ocupando a janela inteira, sem painel.
    //
    // É o que se quer quando a chamada vira "assistir alguém jogar" - e era a
    // única coisa que o cliente em Electron fazia e este não.
    bool telaCheia = false;
    WINDOWPLACEMENT lugarAntes{sizeof(WINDOWPLACEMENT)};
    void alternarTelaCheia();

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
    D2D1_RECT_F btEntrar{};
    // A linha "o que vai no ar" no painel: clicar abre o modal.
    D2D1_RECT_F btEscolher{};
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
                if (y < tema::kAlturaTitulo && !dentro(d->btEngrenagem, x, y) &&
                    x < d->render.largura() - 3 * tema::kLarguraBotaoTitulo) {
                    ::ReleaseCapture();
                    ::SendMessageW(janela, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                    return 0;
                }
                d->clique(x, y);
            }
            return 0;

        // Duplo clique no palco abre e fecha a tela cheia, como em todo
        // reprodutor de vídeo. É onde a mão já está quando dá vontade.
        case WM_LBUTTONDBLCLK:
            if (d) {
                const float x = static_cast<float>(GET_X_LPARAM(l));
                const float y = static_cast<float>(GET_Y_LPARAM(l));
                if (d->telaAtual == Tela::AoVivo && y > tema::kAlturaTitulo &&
                    (d->telaCheia || x < d->render.largura() - tema::kLarguraPainelLateral -
                                             2 * tema::kEspaco)) {
                    d->alternarTelaCheia();
                }
            }
            return 0;

        // Guardar a posição a cada movimento é o que alimenta o realce do que
        // está sob o ponteiro. O arrasto do volume vem junto: sem ele só daria
        // para clicar num ponto, e volume é coisa que se acerta arrastando e
        // ouvindo.
        case WM_MOUSEMOVE:
            if (d) {
                d->mouseX = static_cast<float>(GET_X_LPARAM(l));
                d->mouseY = static_cast<float>(GET_Y_LPARAM(l));
                if (d->arrastandoVolume && (w & MK_LBUTTON)) d->ajustarVolumePor(d->mouseX);

                // O Windows só manda WM_MOUSELEAVE para quem pediu, e o pedido
                // vale uma vez só - por isso é renovado a cada movimento.
                TRACKMOUSEEVENT rastro{sizeof(rastro), TME_LEAVE, janela, 0};
                ::TrackMouseEvent(&rastro);
            }
            return 0;

        // O ponteiro saiu da janela: nada mais está sob ele. Sem isto o último
        // botão apontado ficava aceso para sempre depois que a mão saía.
        case WM_MOUSELEAVE:
            if (d) { d->mouseX = -1; d->mouseY = -1; }
            return 0;

        case WM_MOUSEWHEEL:
            if (d) {
                // A coordenada da roda vem em tela, não em cliente.
                POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                ::ScreenToClient(janela, &p);
                const auto x = static_cast<float>(p.x);
                const auto y = static_cast<float>(p.y);
                const float passos = static_cast<float>(GET_WHEEL_DELTA_WPARAM(w)) / WHEEL_DELTA;

                // Com o modal aberto a roda é dele: rolar o painel atrás de uma
                // janela modal é rolar o que não dá para ver.
                if (d->modalAberto()) {
                    const auto& area = d->areaRolavelModal;
                    if (x >= area.left - 20 && x <= area.right + 20 && y >= area.top &&
                        y <= area.bottom) {
                        const float limite = d->alturaConteudoModal - (area.bottom - area.top);
                        d->rolagemModal -= passos * 48.0f;
                        if (d->rolagemModal < 0) d->rolagemModal = 0;
                        if (limite > 0 && d->rolagemModal > limite) d->rolagemModal = limite;
                        else if (limite <= 0) d->rolagemModal = 0;
                    }
                } else if (x >= d->areaRolavel.left && x <= d->areaRolavel.right &&
                           y >= d->areaRolavel.top && y <= d->areaRolavel.bottom) {
                    d->rolar(passos);
                }
            }
            return 0;

        // Mãozinha em cima do que é clicável. O que decide é o desenho do
        // quadro anterior, que já sabe onde cada botão caiu.
        case WM_SETCURSOR:
            if (d && LOWORD(l) == HTCLIENT && d->sobreClicavel) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            return ::DefWindowProcW(janela, msg, w, l);

        case WM_LBUTTONUP:
            if (d && d->arrastandoVolume) {
                d->arrastandoVolume = false;
                ::ReleaseCapture();
                d->config.volume = d->volumeDaChamada;
                d->config.salvar();
            }
            return 0;

        // F11 entra e sai da tela cheia; Esc só sai. É o par que todo navegador
        // e todo reprodutor usa, e teclar Esc é o primeiro reflexo de quem se
        // vê preso numa janela sem bordas.
        case WM_KEYDOWN:
            if (d) {
                if (w == VK_ESCAPE && d->modalAberto()) { d->fecharModal(); return 0; }
                if (d->telaAtual == Tela::AoVivo) {
                    if (w == VK_F11) { d->alternarTelaCheia(); return 0; }
                    if (w == VK_ESCAPE && d->telaCheia) { d->alternarTelaCheia(); return 0; }
                }
            }
            return ::DefWindowProcW(janela, msg, w, l);

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
    // Sem CS_DBLCLKS o Windows nunca manda WM_LBUTTONDBLCLK: os dois cliques
    // chegam como dois cliques soltos, e o duplo clique do palco não existiria.
    classe.style = CS_DBLCLKS;
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

    // A lista de câmeras é montada agora, mas nenhuma é aberta: enumerar não
    // acende a luzinha de ninguém. A escolhida só é aberta quando a
    // transmissão começa.
    d_->divisoes = d_->config.divisoes;
    d_->telasLigadas = d_->config.telas;

    // Primeira abertura: a tela principal já vem marcada.
    //
    // Sem isto o aplicativo abre sem nada escolhido, e apertar TRANSMITIR não
    // faz nada até a pessoa entrar no modal e marcar alguma coisa - o que é
    // exigir uma decisão antes de mostrar que o programa funciona. Mostrar a
    // tela é o motivo de ele existir; que seja o padrão.
    if (d_->config.telas.empty() && d_->config.cameras.empty()) {
        d_->telasLigadas = {0};
    }

    d_->cameras = CameraCapture::listar();
    for (const auto& c : d_->cameras) {
        info("camera encontrada: {} ({})", c.nome, c.id);
    }
    if (d_->cameras.empty()) info("nenhuma camera no computador");

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

    // A câmera guardada abre junto com o dispositivo, e não só quando a
    // transmissão começa: quem deixou a câmera ligada da última vez espera
    // se ver na tela ao abrir o programa, não depois de apertar TRANSMITIR.
    d_->reabrirCameras();

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
    soltarCamerasDoDispositivo();

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

    reabrirCameras();

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
                // A camera segue aberta fora do ar, para a previa. Fechar aqui
                // e o que apaga a luzinha ao sair do programa.
                d_->soltarCamerasDoDispositivo();
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
        // Antes de capturar: se o monitor mudou de resolução, o conversor e o
        // encoder ainda estão montados para o tamanho de antes.
        if (d_->precisaRemontar) d_->remontarVideo();

        d_->bombearCaptura();

        // As miniaturas do modal so andam enquanto ele esta aberto.
        if (d_->modalAberto()) d_->bombearPreviasDoModal();

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

    // Telas extras primeiro, e cada quadro novo vira uma cópia própria.
    //
    // A textura que a duplicação entrega vale só até a próxima leitura, e ela
    // só entrega quando AQUELA tela muda. Compondo direto da textura
    // emprestada, um monitor parado sumiria do quadro seguinte - piscando entre
    // a imagem e preto conforme a outra tela se mexesse.
    if (copiasExtras.size() != telasExtras.size()) copiasExtras.resize(telasExtras.size());
    for (size_t i = 0; i < telasExtras.size(); ++i) {
        QuadroCapturado extra;
        const auto estado = telasExtras[i]->proximoQuadro(0, extra);
        if (estado == ResultadoQuadro::PrecisaReiniciar) {
            telasExtras[i]->reiniciar();
            continue;
        }
        if (estado != ResultadoQuadro::Ok || !extra.textura) continue;

        D3D11_TEXTURE2D_DESC origem{};
        extra.textura->GetDesc(&origem);
        if (copiasExtras[i]) {
            D3D11_TEXTURE2D_DESC atual{};
            copiasExtras[i]->GetDesc(&atual);
            if (atual.Width != origem.Width || atual.Height != origem.Height) {
                copiasExtras[i].Reset();
            }
        }
        if (!copiasExtras[i]) {
            D3D11_TEXTURE2D_DESC nova = origem;
            nova.Usage = D3D11_USAGE_DEFAULT;
            // Sem flag de ligação: esta textura só é destino de cópia e entrada
            // do Video Processor, e SHADER_RESOURCE aqui faria a placa da AMD
            // recusar a entrada - a mesma armadilha da textura do decodificador.
            nova.BindFlags = 0;
            nova.CPUAccessFlags = 0;
            nova.MiscFlags = 0;
            nova.MipLevels = 1;
            nova.ArraySize = 1;
            tela.dispositivo()->CreateTexture2D(&nova, nullptr, &copiasExtras[i]);
        }
        if (copiasExtras[i]) {
            tela.contexto()->CopyResource(copiasExtras[i].Get(), extra.textura);
        }
        telasExtras[i]->liberarQuadro();
    }

    QuadroCapturado quadro;
    // Prazo curto: a interface não pode ficar esperando a tela mudar, senão
    // clique e digitação engasgam.
    switch (tela.proximoQuadro(4, quadro)) {
        case ResultadoQuadro::Ok: {
            quadrosNoSegundo += 1;

            // A resolução do monitor muda com o programa rodando: jogo que
            // troca o modo de vídeo, monitor reconectado, a pessoa mexendo nas
            // configurações de tela.
            //
            // O conversor, o encoder e o compositor do cursor foram montados
            // para o tamanho de antes. O conversor é o que dá para ver: ele lê
            // uma área maior do que a textura agora tem. Remontar é o único
            // caminho, e não pode ser feito daqui - pararEncodeSomente pega a
            // mesma trava que esta função já está segurando. Fica anotado, e o
            // laço principal refaz antes da próxima passada.
            if (quadro.largura != larguraMontada || quadro.altura != alturaMontada) {
                if (larguraMontada != 0) {
                    info("a captura mudou de {}x{} para {}x{}; refazendo o video",
                         larguraMontada, alturaMontada, quadro.largura, quadro.altura);
                    precisaRemontar = true;
                }
                larguraMontada = quadro.largura;
                alturaMontada = quadro.altura;

                // O compositor do cursor tem o tamanho velho gravado nele.
                cursorPronto = false;
            }

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

                // Tudo que entra no quadro, na ordem em que a composição foi
                // montada: a tela principal, as extras e a câmera por último.
                //
                // A câmera e as telas extras podem estar nulas: elas entregam
                // no ritmo delas, e uma entrada nula é pulada em vez de abrir
                // buraco preto. O resto do quadro sai igual.
                // A ORDEM tem de ser exatamente a que o comecarTransmissao usou
                // para montar os pedaços: tela principal (se ligada), telas
                // extras, câmeras.
                std::vector<ID3D11Texture2D*> entradas;
                entradas.reserve(2 + telasExtras.size() + camerasAbertas.size());
                if (telaLigada(monitorEscolhido)) entradas.push_back(quadroAtual);
                for (auto& copia : copiasExtras) entradas.push_back(copia.Get());
                for (auto& c : camerasAbertas) {
                    if (c->captura.ativa()) entradas.push_back(c->captura.quadro());
                }

                if (auto* nv12 = conversor.compor(entradas)) {
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

    const Qualidade& q = kQualidades[qualidadeEscolhida];

    // Telas extras abrem ANTES da conta do tamanho: é o tamanho delas que entra
    // na soma.
    abrirTelasExtras();

    // Uma entrada por imagem que vai no quadro, na ordem em que serão
    // compostas e na MESMA ordem em que bombearCaptura vai entregá-las.
    struct Fonte {
        double largura;   // já normalizada para a altura comum
        double altura;
        uint32_t graus;
    };
    std::vector<Fonte> fontes;

    const bool principalLigada = telaLigada(monitorEscolhido);
    if (principalLigada) {
        const auto& m = tela.monitor();
        fontes.push_back({static_cast<double>(m.largura), static_cast<double>(m.altura), m.graus});
    }
    for (const auto& extra : telasExtras) {
        if (!extra->capturando()) continue;
        const auto& e = extra->monitor();
        fontes.push_back({static_cast<double>(e.largura), static_cast<double>(e.altura), e.graus});
    }

    const size_t quantasTelas = fontes.size();

    // As câmeras entram como fonte quando não há tela nenhuma: aí são elas a
    // imagem. Com tela, elas vão nos cantos, o que é calculado depois.
    std::vector<CameraAberta*> camerasNoQuadro;
    for (auto& c : camerasAbertas) {
        if (c->captura.ativa()) camerasNoQuadro.push_back(c.get());
    }

    if (quantasTelas == 0 && camerasNoQuadro.empty()) {
        aviso = L"Escolha o que transmitir.";
        return false;
    }

    if (quantasTelas == 0) {
        for (auto* c : camerasNoQuadro) {
            fontes.push_back({static_cast<double>(c->captura.largura()),
                              static_cast<double>(c->captura.altura()), 0});
        }
    }

    // Larguras alinhadas por uma altura comum.
    //
    // Lado a lado sem isso, uma imagem fica flutuando com faixa preta em cima
    // ou embaixo. A altura comum é a maior das lógicas; cada imagem contribui
    // com a largura que a proporção dela pede nessa altura.
    double alturaComum = 0;
    for (const auto& f : fontes) alturaComum = (std::max)(alturaComum, f.altura);
    if (alturaComum <= 0) {
        aviso = L"Não foi possível medir o que transmitir.";
        return false;
    }

    double larguraTotal = 0;
    for (auto& f : fontes) {
        f.largura = f.largura * alturaComum / f.altura;
        larguraTotal += f.largura;
    }

    // A saída respeita a proporção do conjunto, dentro do que a qualidade
    // permite. E nunca AMPLIA: sem o teto em 1, um monitor 1024x768 no preset
    // de 1080p era esticado para 1440x1080 - mais pixels para codificar, mais
    // banda gasta, e a mesma imagem, borrada. A qualidade é um limite, não uma
    // meta.
    const double escala =
        (std::min)(1.0, (std::min)(static_cast<double>(q.largura) / larguraTotal,
                                   static_cast<double>(q.altura) / alturaComum));
    const uint32_t saidaL = static_cast<uint32_t>(larguraTotal * escala) & ~1u;
    const uint32_t saidaA = static_cast<uint32_t>(alturaComum * escala) & ~1u;

    // A ENTRADA do conversor é o tamanho FÍSICO da primeira imagem - é só uma
    // dica de tamanho para o Video Processor, e cada fluxo diz o resto.
    uint32_t entradaL = saidaL;
    uint32_t entradaA = saidaA;
    if (principalLigada) {
        entradaL = tela.monitor().larguraFisica();
        entradaA = tela.monitor().alturaFisica();
    } else if (!camerasNoQuadro.empty()) {
        entradaL = camerasNoQuadro.front()->captura.largura();
        entradaA = camerasNoQuadro.front()->captura.altura();
    }

    if (!conversor.iniciar(tela.dispositivo(), tela.contexto(), entradaL, entradaA, saidaL, saidaA,
                           ColorConverter::Saida::Nv12, 0)) {
        aviso = L"Não foi possível preparar a conversão de cor.";
        return false;
    }

    // Onde cada imagem cai. O giro vai por pedaço: um monitor em pé ao lado de
    // um deitado é o caso comum de quem tem dois.
    std::vector<ColorConverter::Pedaco> pedacos;
    double x = 0;
    for (const auto& f : fontes) {
        ColorConverter::Pedaco pedaco;
        pedaco.esquerda = static_cast<int32_t>(x * escala);
        pedaco.topo = 0;
        pedaco.direita = static_cast<int32_t>((x + f.largura) * escala);
        pedaco.baixo = static_cast<int32_t>(saidaA);
        pedaco.graus = f.graus;
        pedacos.push_back(pedaco);
        x += f.largura;
    }

    // Com tela, as câmeras vão numa fileira no canto de baixo à direita, da
    // direita para a esquerda. Sem tela elas já são as fontes acima.
    if (quantasTelas > 0 && !camerasNoQuadro.empty()) {
        // Quanto mais coisa no quadro, menor cada câmera: um quarto da largura
        // numa composição de dois monitores já é meia tela.
        const double fatia = 4.0 + 2.0 * static_cast<double>(quantasTelas - 1) +
                             1.0 * static_cast<double>(camerasNoQuadro.size() - 1);
        const int32_t margem = static_cast<int32_t>(saidaL / 48);
        const int32_t larguraCam = static_cast<int32_t>(saidaL / fatia);

        int32_t direita = static_cast<int32_t>(saidaL) - margem;
        for (auto* c : camerasNoQuadro) {
            const int32_t alturaCam = static_cast<int32_t>(static_cast<double>(larguraCam) *
                                                           c->captura.altura() /
                                                           c->captura.largura());
            ColorConverter::Pedaco canto;
            canto.direita = direita;
            canto.baixo = static_cast<int32_t>(saidaA) - margem;
            canto.esquerda = direita - larguraCam;
            canto.topo = canto.baixo - alturaCam;
            pedacos.push_back(canto);
            direita = canto.esquerda - margem / 2;
        }
    }

    if (pedacos.size() > 1 && !conversor.prepararComposicao(pedacos)) {
        // A placa não deu conta de compor. Cai para a primeira imagem sozinha,
        // que é melhor do que não transmitir nada.
        gl::aviso("nao foi possivel compor {} imagens; indo so com a primeira", pedacos.size());
        fecharTelasExtras();
        conversor.desligarComposicao();
    }

    info("transmitindo {} tela(s) e {} camera(s): {}x{}", quantasTelas, camerasNoQuadro.size(),
         saidaL, saidaA);
    return montarEncoder(saidaL, saidaA, q);
}

// Encoder, áudio e rede: a parte da transmissão que não depende de a imagem
// vir de uma tela, de duas ou da câmera.
//
// Ficava tudo dentro do comecarTransmissao. Separar foi o que permitiu o
// caminho de "só a câmera" existir sem repetir noventa linhas nem encher a
// função de casos especiais.
bool Aplicacao::Interno::montarEncoder(uint32_t largura, uint32_t altura, const Qualidade& q) {
    ConfigEncoder cfg;
    cfg.largura = largura;
    cfg.altura = altura;
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

    // Avisa que a transmissão acabou, em vez de só parar de mandar pacote.
    //
    // Sem o aviso, quem assiste fica com o último quadro congelado até o
    // próprio tempo sem pacote expirar - e no celular isso demora. O servidor
    // não manda esse recado por conta própria: quem publica é que avisa, e é o
    // que o cliente do navegador já faz.
    //
    // O identificador é o mesmo que o servidor monta para a stream ao
    // encaminhar: "greenlabs-" mais os oito primeiros do dono. É por ele que o
    // outro lado acha o cartão.
    if (!meuId.empty()) {
        Json fim = Json::objeto();
        fim["type"] = Json{std::string("stream-ended")};
        fim["streamId"] = Json{"greenlabs-" + meuId.substr(0, 8)};

        std::vector<std::string> destinos;
        {
            std::lock_guard trava(travaPares);
            for (const auto& p : pares) {
                if (p.id != meuId) destinos.push_back(p.id);
            }
        }
        for (const auto& id : destinos) sinal.enviarPara(id, fim);
    }

    // Em modo retransmissor a conexão com o servidor NÃO cai aqui.
    //
    // Ela é o caminho dos dois sentidos: derrubá-la ao parar de transmitir
    // derruba junto o que se RECEBE, e apertar transmitir de novo não tinha
    // mais por onde sair - dava para ver no diagnóstico "conexao: sem ninguem"
    // com a transmissão ligada. Quem parou fica sem imagem para os outros
    // sozinho, porque o fluxo seca e eles tiram da lista pelo tempo sem pacote.
    //
    // Em malha é diferente: lá a conexão existe por causa da transmissão, e sem
    // ela não há o que manter de pé.
    if (!modoSfu.load()) {
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

// Tela cheia de verdade: a janela cobre o monitor em que ela está.
//
// Não é maximizar. Maximizada, a barra de tarefas continua na frente e a barra
// de título continua ocupando a faixa de cima - e quem pede tela cheia quer a
// imagem, não a moldura. O lugar de antes fica guardado para a volta ser exata,
// inclusive quando a janela estava maximizada.
void Aplicacao::Interno::alternarTelaCheia() {
    if (!telaCheia) {
        ::GetWindowPlacement(janela, &lugarAntes);

        // O monitor de onde a janela está, e não o primário: em duas telas,
        // levar a janela para a outra ao entrar em tela cheia é o tipo de coisa
        // que faz a pessoa achar que o aplicativo travou.
        HMONITOR monitor = ::MonitorFromWindow(janela, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(MONITORINFO)};
        if (!::GetMonitorInfoW(monitor, &info)) return;

        telaCheia = true;
        ::SetWindowPos(janela, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                       info.rcMonitor.right - info.rcMonitor.left,
                       info.rcMonitor.bottom - info.rcMonitor.top,
                       SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        telaCheia = false;
        ::SetWindowPlacement(janela, &lugarAntes);
        ::SetWindowPos(janela, nullptr, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                           SWP_FRAMECHANGED);
    }
}

// Acender ou apagar a câmera.
//
// Ela abre na hora em que é escolhida, e não só quando a transmissão começa: a
// pessoa precisa se ver para saber se está enquadrada, e "clico e não acontece
// nada até eu transmitir" é o mesmo que não ter câmera. Escolher "Desligada"
// fecha o dispositivo de verdade - a luzinha apaga.
// Abre e fecha câmeras até a lista de abertas bater com a escolhida.
//
// A câmera abre na hora em que é MARCADA, e não quando a transmissão começa:
// a pessoa precisa se ver para saber se está enquadrada, e "marco e não
// acontece nada até eu transmitir" é o mesmo que não ter câmera. Desmarcar
// fecha o dispositivo de verdade - a luzinha apaga.
void Aplicacao::Interno::aplicarCameras(const std::vector<std::string>& escolhidas) {
    // Fecha o que saiu da lista.
    for (auto it = camerasAbertas.begin(); it != camerasAbertas.end();) {
        const bool continua =
            std::find(escolhidas.begin(), escolhidas.end(), (*it)->id) != escolhidas.end();
        if (continua) {
            ++it;
            continue;
        }
        (*it)->captura.parar();
        render.esquecerVideo("cam:" + (*it)->id);
        it = camerasAbertas.erase(it);
    }

    // Abre o que entrou, na ordem da escolha - é a mesma ordem em que elas vão
    // aparecer no quadro.
    std::vector<std::string> abertas;
    for (const auto& id : escolhidas) {
        if (auto* ja = cameraAberta(id)) {
            abertas.push_back(ja->id);
            continue;
        }
        auto nova = std::make_unique<CameraAberta>();
        nova->id = id;
        for (const auto& c : cameras) {
            if (c.id == id) nova->nome = c.nome;
        }
        if (!nova->captura.iniciar(id, tela.dispositivo(), tela.contexto())) {
            // Câmera ocupada por outro programa, ou desligada entre a listagem
            // e o clique. Sai da escolha em vez de ficar marcada mostrando nada.
            aviso = L"Não foi possível abrir \"" + paraW(nova->nome) +
                    L"\". Ela pode estar em uso por outro programa.";
            continue;
        }
        abertas.push_back(id);
        camerasAbertas.push_back(std::move(nova));
    }

    // A lista guardada é a das que REALMENTE abriram: deixar na configuração
    // uma câmera que falha toda vez faria o aviso voltar a cada abertura.
    camerasEscolhidas = abertas;
    config.cameras = abertas;
    config.salvar();

    // Só depois: é o conversor do encoder que precisa saber quantas entradas
    // existem agora, e ele só existe transmitindo.
    reiniciarEncode();
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

            // Em modo retransmissor o peerId é sempre "sfu": as pessoas não
            // falam entre si, então o nome não chega por metadado. Ele sai do
            // próprio identificador da faixa, que o servidor monta como
            // "greenlabs-<8 primeiros do dono>-video".
            //
            // Antes daqui saía "o primeiro participante que não sou eu", o que
            // acerta com duas pessoas na sala e erra com três - e numa chamada
            // de grupo o nome errado embaixo da tela é pior do que nome nenhum.
            if (peerId == kIdDoSFU) {
                std::lock_guard tp(travaPares);
                //
                // Só serve o identificador com a forma que o servidor monta:
                // "greenlabs-" mais oito caracteres do dono. A faixa que NÓS
                // publicamos chega aqui como "greenlabs-tela", que não tem essa
                // forma - e tomá-la por um dono poria o nome de alguém embaixo
                // da nossa própria imagem.
                if (t->dono.empty() && faixaId.rfind("greenlabs-", 0) == 0 &&
                    faixaId.size() >= 18) {
                    const std::string prefixo = faixaId.substr(10, 8);
                    for (const auto& p : pares) {
                        if (p.id != meuId && p.id.rfind(prefixo, 0) == 0) {
                            t->dono = p.id;
                            t->nome = p.nome;
                            break;
                        }
                    }
                }
                if (t->nome.empty()) {
                    for (const auto& p : pares) {
                        if (p.id != meuId) { t->nome = p.nome; break; }
                    }
                    if (t->nome.empty()) t->nome = "Alguem na sala";
                }
            } else if (t->nome.empty()) {
                t->dono = peerId;
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

// Troca o monitor PRINCIPAL: o que define de qual placa vem o dispositivo D3D.
//
// É a operação cara. Todo o resto - encoder, decodificador, interface, câmera -
// vive nesse dispositivo, e trocá-lo significa refazer tudo. Por isso ligar uma
// segunda tela não passa por aqui: a extra entra por iniciarCom, no dispositivo
// que já existe.
void Aplicacao::Interno::trocarPrincipal(int novo) {
    const bool estavaTransmitindo = transmitindo;
    // So o encode: a conexao com o servidor continua de pe. Antes
    // isto chamava pararTransmissao, que limpa o mapa de conexoes -
    // trocar de monitor derrubava a chamada inteira e a
    // renegociacao seguinte falhava.
    pararEncodeSomente();

    // A câmera guarda texturas do dispositivo D3D atual, e trocar
    // de monitor cria outro. Ela volta logo abaixo, no novo.
    soltarCamerasDoDispositivo();

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
    reabrirCameras();
    if (estavaTransmitindo) comecarTransmissao();
}

void Aplicacao::Interno::clique(float x, float y) {
    // O modal come todos os cliques enquanto está aberto. É o que faz dele um
    // modal: nada atrás dele responde.
    if (modal == Modal::Config) {
        if (dentro(btModalFechar, x, y) || dentro(btConfigConcluir, x, y)) {
            // Concluir é fechar salvando: o que está nos campos vira o que
            // vale. Dois botões que fazem quase a mesma coisa confundiriam.
            config.nome = paraUtf8(campoNome.valor);
            config.servidor = paraUtf8(campoServidor.valor);
            config.sala = paraUtf8(campoSala.valor);
            config.salvar();
            fecharModal();
            return;
        }
        for (size_t i = 0; i < btAbasConfig.size(); ++i) {
            if (dentro(btAbasConfig[i], x, y)) {
                abaConfig = static_cast<int>(i);
                return;
            }
        }
        if (abaConfig == 0) {
            for (Campo* campo : {&campoNome, &campoServidor, &campoSala}) {
                campo->focado = dentro(campo->area, x, y);
                if (!campo->focado) campo->tudoSelecionado = false;
            }
            if (dentro(btSalvarPadrao, x, y)) {
                config.nome = paraUtf8(campoNome.valor);
                config.servidor = paraUtf8(campoServidor.valor);
                config.sala = paraUtf8(campoSala.valor);
                config.lembrarServidor(config.servidor);
                config.salvar();
                avisoConfig = L"Salvo. É o que vai abrir da próxima vez.";
                return;
            }
            if (dentro(btRestaurar, x, y)) {
                // Só as preferências. Não mexe em quem está na sala nem derruba
                // a chamada: restaurar o que se digita não é sair de onde se
                // está.
                const Config limpo;
                config.qualidade = limpo.qualidade;
                config.audio = limpo.audio;
                config.volume = limpo.volume;
                config.telas = {0};
                config.cameras.clear();
                config.salvar();

                qualidadeEscolhida = config.qualidade;
                audioLigado = config.audio;
                volumeDaChamada = config.volume;
                alto.definirVolume(static_cast<float>(volumeDaChamada) / 100.0f);
                telasLigadas = config.telas;
                aplicarCameras({});
                avisoConfig = L"Preferências restauradas.";
                return;
            }
            return;
        }
        for (size_t i = 0; i < btRemoverServidor.size() && i < config.servidores.size(); ++i) {
            if (dentro(btRemoverServidor[i], x, y)) {
                config.servidores.erase(config.servidores.begin() + static_cast<long>(i));
                config.salvar();
                return;
            }
        }
        for (size_t i = 0; i < btServidoresConfig.size() && i < config.servidores.size(); ++i) {
            if (dentro(btServidoresConfig[i], x, y)) {
                campoServidor.valor = paraW(config.servidores[i]);
                config.servidor = config.servidores[i];
                config.salvar();
                abaConfig = 0;
                return;
            }
        }
        return;
    }

    if (modal == Modal::Fonte) {
        if (dentro(btModalFechar, x, y)) { fecharModal(); return; }
        // Trocar de aba volta a rolagem ao topo: a lista e outra.
        if (dentro(btAbaTelas, x, y)) { abaModal = 0; rolagemModal = 0; return; }
        if (dentro(btAbaCameras, x, y)) { abaModal = 1; rolagemModal = 0; return; }
        if (dentro(btModalAudio, x, y)) { audioNoModal = !audioNoModal; return; }

        for (size_t i = 0; i < btQualidades.size(); ++i) {
            if (dentro(btQualidades[i], x, y)) {
                qualidadeNoModal = static_cast<int>(i);
                return;
            }
        }

        for (size_t i = 0; i < btCartoesModal.size(); ++i) {
            if (!dentro(btCartoesModal[i], x, y)) continue;
            if (abaModal == 0) {
                const int indice = static_cast<int>(i);
                const auto achou = std::find(telasNoModal.begin(), telasNoModal.end(), indice);
                if (achou != telasNoModal.end()) telasNoModal.erase(achou);
                else telasNoModal.push_back(indice);
            } else if (i < cameras.size()) {
                const std::string& id = cameras[i].id;
                const auto achou = std::find(camerasNoModal.begin(), camerasNoModal.end(), id);
                if (achou != camerasNoModal.end()) {
                    camerasNoModal.erase(achou);
                } else {
                    camerasNoModal.push_back(id);
                }
                // A câmera abre na hora em que é marcada: sem isso o cartão
                // ficaria marcado mostrando "sem imagem ainda", e a pessoa não
                // teria como se enquadrar antes de aparecer.
                aplicarCameras(camerasNoModal);
                // aplicarCameras devolve so as que realmente abriram: uma que
                // falhou nao pode continuar marcada no modal.
                camerasNoModal = camerasEscolhidas;
            }
            return;
        }

        if (dentro(btModalConfirmar, x, y) &&
            !(telasNoModal.empty() && camerasNoModal.empty())) {
            confirmarModal();
        }
        return;
    }

    if (dentro(btFechar, x, y)) {
        ::PostMessageW(janela, WM_CLOSE, 0, 0);
        return;
    }
    if (dentro(btMinimizar, x, y)) {
        ::ShowWindow(janela, SW_MINIMIZE);
        return;
    }

    // A engrenagem fica na BARRA, fora da area rolavel do painel - e mais
    // abaixo ha um return antecipado para tudo que esta fora dela. Testar aqui,
    // junto dos outros botoes da barra, e o que faz o clique chegar: antes ele
    // era engolido por aquele return e a engrenagem simplesmente nao respondia.
    // A barra de ações e o palco ficam FORA da área rolável do painel, e mais
    // abaixo há um return antecipado para tudo que está fora dela. Testar aqui,
    // junto dos botões da janela, é o que faz esses cliques chegarem - foi
    // exatamente por estar depois daquele return que a engrenagem não respondia.
    if (dentro(btEngrenagem, x, y)) {
        abrirConfig();
        return;
    }
    if (dentro(btBarraFonte, x, y)) {
        abrirModal();
        return;
    }
    if (dentro(btBarraCamera, x, y)) {
        // O mesmo modal, ja na aba das cameras: quem clicou na camera quer a
        // camera, nao a lista de telas.
        abrirModal();
        abaModal = 1;
        return;
    }
    if (dentro(btBarraTransmitir, x, y)) {
        // Parar e imediato; comecar passa pela escolha do que vai no ar.
        if (transmitindo) pararTransmissao();
        else abrirModal();
        return;
    }
    if (dentro(btBarraSair, x, y)) {
        if (conectado.load()) {
            pararTransmissao();
            sinal.sair();
            conectado.store(false);
            telaAtual = Tela::Entrada;
        } else {
            conectar();
        }
        return;
    }
    for (size_t i = 0; i < btDivisoes.size(); ++i) {
        if (!dentro(btDivisoes[i], x, y)) continue;
        divisoes = (i == 0) ? 1 : (i == 1) ? 2 : 4;
        config.divisoes = divisoes;
        config.salvar();
        return;
    }
    if (dentro(btExpandirQuadro, x, y)) {
        alternarTelaCheia();
        return;
    }
    // Clicar num quadro põe aquela transmissão em primeiro - é o que decide
    // quem aparece quando o palco está dividido em menos quadros do que há
    // gente transmitindo.
    for (size_t i = 0; i < btQuadros.size() && i < chavesQuadros.size(); ++i) {
        if (!dentro(btQuadros[i], x, y)) continue;
        std::lock_guard t(travaTransmissoes);
        noPalco = chavesQuadros[i];
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

    // Em tela cheia não há painel: qualquer clique que não seja nos controles
    // da barra (que também não existe) é clique no palco, e o palco não faz
    // nada com um clique simples. Sair é duplo clique, Esc ou F11.
    if (telaCheia) return;

    // O que rola só aceita clique dentro da própria janela de rolagem. Sem
    // isto, um item que rolou para debaixo dos botões continuava sendo clicado
    // - invisível, mas ativo.
    const bool naAreaRolavel =
        y >= areaRolavel.top && y <= areaRolavel.bottom;
    if (!naAreaRolavel) return;

    if (dentro(btEscolher, x, y)) {
        abrirModal();
        return;
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

}

// ---------------------------------------------------------------- desenho

bool Aplicacao::Interno::desenharBotao(const D2D1_RECT_F& area, const std::wstring& rotulo,
                                       bool destaque, bool habilitado) {
    // Apontar um botão desabilitado não acende nada e não vira mãozinha: o
    // ponteiro é a primeira coisa que diz se vale a pena clicar.
    const bool sob = habilitado && apontando(area);

    const auto fundo = destaque ? tema::kVerdeSuave : (sob ? tema::kPainel3 : tema::kPainel2);
    const auto borda = destaque ? tema::kVerdeLinha : (sob ? tema::kVerdeLinha : tema::kLinha);
    const auto corTexto = !habilitado ? tema::kApagado : (destaque ? tema::kVerde : tema::kTexto);

    render.retangulo(area, fundo, tema::kRaioBotao);
    render.contorno(area, borda, tema::kRaioBotao);
    render.texto(rotulo, area, corTexto, Fonte::Botao, DWRITE_TEXT_ALIGNMENT_CENTER);
    return habilitado;
}


// O campo sem o rótulo por cima.
//
// O desenharCampo original escreve o nome do campo ACIMA da área, o que serve
// na tela de entrada, onde ele mesmo posiciona tudo. No modal o rótulo já vem
// desenhado separado, e chamar o outro escreveria duas vezes.
void Aplicacao::Interno::desenharCampoSimples(Campo& campo) {
    render.retangulo(campo.area, tema::kPainel2, tema::kRaioBotao);
    render.contorno(campo.area, campo.focado ? tema::kVerdeLinha : tema::kLinha,
                    tema::kRaioBotao);

    const auto interna = D2D1::RectF(campo.area.left + 14, campo.area.top, campo.area.right - 14,
                                     campo.area.bottom);
    if (campo.valor.empty()) {
        render.texto(campo.dica, interna, tema::kApagado, Fonte::Corpo);
        return;
    }

    if (campo.tudoSelecionado) {
        const float larguraTexto = render.larguraDoTexto(campo.valor, Fonte::Corpo);
        render.retangulo(D2D1::RectF(interna.left - 3, interna.top + 9,
                                     interna.left + larguraTexto + 3, interna.bottom - 9),
                         tema::kVerdeSuave, 4);
    }
    const bool piscar =
        campo.focado && !campo.tudoSelecionado && (::GetTickCount64() / 500) % 2 == 0;
    render.texto(campo.valor + (piscar ? L"|" : L""), interna, tema::kTexto, Fonte::Corpo);
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


// A barra de ações, num cartão logo abaixo da barra de título.
//
// São duas fileiras de propósito, e é o desenho do cliente em Electron: em
// cima o nome da janela e os botões do Windows, que são do sistema; embaixo o
// que é do aplicativo - a marca, o estado da sala e as ações. Misturar os dois
// numa fileira só faz o botão de fechar ficar do lado do de transmitir, e um
// deles vai ser clicado por engano.
void Aplicacao::Interno::desenharBarraDeAcoes() {
    const float larg = render.largura();
    const float topo = tema::kAlturaTitulo + tema::kEspaco;
    const auto barra = D2D1::RectF(tema::kEspaco, topo, larg - tema::kEspaco,
                                   topo + tema::kAlturaAcoes);

    render.retangulo(barra, tema::kPainel, 16);
    render.contorno(barra, tema::kLinha, 16);

    // ---- esquerda: a marca e o estado
    const auto selo = D2D1::RectF(barra.left + 10, barra.top + 9, barra.left + 44,
                                  barra.bottom - 9);
    render.retangulo(selo, tema::kVerdeSuave, 10);
    render.contorno(selo, tema::kVerdeLinha, 10);
    render.logo(D2D1::RectF(selo.left + 5, selo.top + 5, selo.right - 5, selo.bottom - 5));

    const bool ligado = conectado.load();
    render.retangulo(D2D1::RectF(barra.left + 56, (barra.top + barra.bottom) / 2 - 4,
                                 barra.left + 64, (barra.top + barra.bottom) / 2 + 4),
                     ligado ? tema::kVerde : tema::kApagado, 4);

    std::wstring estado = L"Desconectado";
    if (ligado) {
        size_t quantas = 0;
        {
            std::lock_guard trava(travaPares);
            quantas = pares.size() + 1;
        }
        estado = L"Conectado em " + campoSala.valor + L"  (" + std::to_wstring(quantas) + L")";
    }
    render.texto(estado, D2D1::RectF(barra.left + 72, barra.top, barra.right - 400, barra.bottom),
                 ligado ? tema::kTexto : tema::kApagado, Fonte::Botao);

    // ---- direita: as ações, montadas da borda para dentro
    float x = barra.right - 10;

    auto botao = [&](float largura) {
        const auto area = D2D1::RectF(x - largura, barra.top + 8, x, barra.bottom - 8);
        x = area.left - 6;
        return area;
    };

    // Engrenagem.
    btEngrenagem = botao(36);
    {
        const bool sob = apontando(btEngrenagem);
        render.retangulo(btEngrenagem, sob ? tema::kPainel3 : tema::kPainel2, 10);
        render.contorno(btEngrenagem, sob ? tema::kLinhaForte : tema::kLinha, 10);
        icone::engrenagem(render, btEngrenagem, sob ? tema::kTexto : tema::kApagado, 16.0f);
    }

    // Câmera e tela: os dois abrem o mesmo modal, cada um na aba dele. É o
    // mesmo par de botões da barra do Electron.
    btBarraCamera = botao(36);
    {
        const bool sob = apontando(btBarraCamera);
        render.retangulo(btBarraCamera, sob ? tema::kPainel3 : tema::kPainel2, 10);
        render.contorno(btBarraCamera, sob ? tema::kLinhaForte : tema::kLinha, 10);
        icone::camera(render, btBarraCamera,
                      camerasVivas() > 0 ? tema::kVerde : (sob ? tema::kTexto : tema::kApagado),
                      15.0f);
    }

    btBarraFonte = botao(36);
    {
        const bool sob = apontando(btBarraFonte);
        render.retangulo(btBarraFonte, sob ? tema::kPainel3 : tema::kPainel2, 10);
        render.contorno(btBarraFonte, sob ? tema::kLinhaForte : tema::kLinha, 10);
        icone::monitor(render, btBarraFonte,
                       !telasLigadas.empty() ? tema::kVerde : (sob ? tema::kTexto : tema::kApagado),
                       15.0f);
    }

    // Divisão do palco: três botões grudados, num trilho só - o layout-picker.
    {
        const float largura = 3 * 32 + 8;
        const auto trilho = D2D1::RectF(x - largura, barra.top + 8, x, barra.bottom - 8);
        x = trilho.left - 6;
        render.retangulo(trilho, tema::kPainel2, 10);
        render.contorno(trilho, tema::kLinha, 10);

        btDivisoes.clear();
        for (int i = 0; i < 3; ++i) {
            const auto area = D2D1::RectF(trilho.left + 4 + static_cast<float>(i) * 32,
                                          trilho.top + 4, trilho.left + 4 + static_cast<float>(i + 1) * 32,
                                          trilho.bottom - 4);
            const int quantas = (i == 0) ? 1 : (i == 1) ? 2 : 4;
            const bool ativa = divisoes == quantas;
            const bool sob = !ativa && apontando(area);
            if (ativa) render.retangulo(area, tema::kVerdeSuave, 8);
            else if (sob) render.retangulo(area, tema::kPainel3, 8);
            const auto cor = ativa ? tema::kVerde : (sob ? tema::kTexto : tema::kApagado);
            if (i == 0) icone::umQuadro(render, area, cor);
            else if (i == 1) icone::doisQuadros(render, area, cor);
            else icone::quatroQuadros(render, area, cor);
            btDivisoes.push_back(area);
        }
    }

    // Transmitir e parar, na barra: e a acao principal e precisa de peso.
    btBarraTransmitir = botao(transmitindo ? 104.0f : 128.0f);
    {
        const bool sob = apontando(btBarraTransmitir);
        const auto fundo = transmitindo ? (sob ? tema::kVermelho : tema::kVermelhoSuave)
                                        : (sob ? tema::kVerde : tema::kVerdeForte);
        render.retangulo(btBarraTransmitir, fundo, 10);
        const auto corTexto = transmitindo ? tema::kTexto : tema::kFundo;
        const std::wstring rotulo = transmitindo ? L"PARAR" : L"TRANSMITIR";
        const float larguraTexto = render.larguraDoTexto(rotulo, Fonte::Pequena);
        const float meio = (btBarraTransmitir.left + btBarraTransmitir.right) / 2;
        const float inicio = meio - (larguraTexto + 22) / 2;
        const auto areaIcone =
            D2D1::RectF(inicio, btBarraTransmitir.top, inicio + 16, btBarraTransmitir.bottom);
        if (transmitindo) icone::parar(render, areaIcone, corTexto, 10.0f);
        else icone::transmitir(render, areaIcone, corTexto, 14.0f, 1.6f);
        render.texto(rotulo,
                     D2D1::RectF(inicio + 22, btBarraTransmitir.top, btBarraTransmitir.right,
                                 btBarraTransmitir.bottom),
                     corTexto, Fonte::Pequena);
    }

    // Sair da sala, na barra, como no Electron: a porta vermelha quando se está
    // dentro, a tomada verde quando não. É o mesmo botão trocando de papel -
    // sempre no mesmo lugar, que é o que a mão decora.
    btBarraSair = botao(36);
    {
        const bool sob = apontando(btBarraSair);
        if (ligado) {
            render.retangulo(btBarraSair, sob ? tema::kVermelho : tema::kVermelhoSuave, 10);
            icone::porta(render, btBarraSair, sob ? tema::kTexto : tema::kFundo, 16.0f, 1.6f);
        } else {
            render.retangulo(btBarraSair, sob ? tema::kVerde : tema::kVerdeForte, 10);
            icone::tomada(render, btBarraSair, tema::kFundo, 15.0f, 1.6f);
        }
    }

    // Pastilha do ping.
    if (ligado) {
        const std::wstring texto = std::to_wstring(sinal.pingMs()) + L"ms";
        const float largura = render.larguraDoTexto(texto, Fonte::Pequena) + 26;
        const auto pastilha = botao(largura);
        render.retangulo(pastilha, tema::kVerdeSuave, 9);
        render.contorno(pastilha, tema::kVerdeLinha, 9);
        icone::aoVivo(render,
                      D2D1::RectF(pastilha.left + 8, pastilha.top, pastilha.left + 16,
                                  pastilha.bottom),
                      tema::kVerde, 6.0f);
        render.texto(texto,
                     D2D1::RectF(pastilha.left + 18, pastilha.top, pastilha.right - 6,
                                 pastilha.bottom),
                     tema::kVerde, Fonte::Pequena);
    }
}

void Aplicacao::Interno::desenharBarraTitulo() {
    const float larg = render.largura();
    render.retangulo(D2D1::RectF(0, 0, larg, tema::kAlturaTitulo), tema::kPainel);
    render.linha(0, tema::kAlturaTitulo, larg, tema::kAlturaTitulo, tema::kLinha);

    render.logo(D2D1::RectF(12, 7, 12 + 24, tema::kAlturaTitulo - 7));
    render.texto(L"GreenLabs", D2D1::RectF(44, 0, 220, tema::kAlturaTitulo), tema::kTexto,
                 Fonte::Botao);
    render.texto(L"v" GREENLABS_VERSAO_W, D2D1::RectF(124, 0, 200, tema::kAlturaTitulo),
                 tema::kApagado, Fonte::Pequena);

    const float b = tema::kLarguraBotaoTitulo;


    btFechar = D2D1::RectF(larg - b, 0, larg, tema::kAlturaTitulo);
    btMaximizar = D2D1::RectF(larg - 2 * b, 0, larg - b, tema::kAlturaTitulo);
    btMinimizar = D2D1::RectF(larg - 3 * b, 0, larg - 2 * b, tema::kAlturaTitulo);

    // Realce nos tres, como em qualquer barra de titulo do Windows: o de fechar
    // em vermelho, os outros discretos.
    if (apontando(btMinimizar)) render.retangulo(btMinimizar, tema::kPainel3);
    if (apontando(btMaximizar)) render.retangulo(btMaximizar, tema::kPainel3);
    const bool sobFechar = apontando(btFechar);
    if (sobFechar) render.retangulo(btFechar, tema::kVermelho);

    icone::minimizar(render, btMinimizar, tema::kApagado);
    if (::IsZoomed(janela)) icone::restaurar(render, btMaximizar, tema::kApagado);
    else icone::maximizar(render, btMaximizar, tema::kApagado);
    icone::fechar(render, btFechar, sobFechar ? tema::kTexto : tema::kVermelho);
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

    // O aviso vem ANTES da lista, dentro do cartão.
    //
    // Ele morava embaixo do cartão, no mesmo lugar do rótulo "SERVIDORES
    // SALVOS" - as duas frases se sobrepunham, e "Conectando..." aparecia
    // escrito por cima do rótulo. Aqui ele fica onde a pessoa está olhando: no
    // botão que acabou de apertar.
    if (!aviso.empty()) {
        render.texto(aviso, D2D1::RectF(x + 36, y + 432, x + largCartao - 36, y + 456),
                     aviso.rfind(L"Conectando", 0) == 0 ? tema::kApagado : tema::kVermelho,
                     Fonte::Pequena, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

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

    // Em tela cheia não há painel nem barras: o palco é a janela. Os controles
    // não somem por elegância - é que numa tela cheia eles ficariam por cima da
    // imagem, e a imagem é o motivo de alguém pedir tela cheia.
    const float topo =
        telaCheia ? 0.0f : tema::kAlturaTitulo + 2 * tema::kEspaco + tema::kAlturaAcoes;
    const float painelX =
        telaCheia ? larg : larg - tema::kLarguraPainelLateral - tema::kEspaco;

    // Fotografia das transmissões deste quadro. Copiar o essencial sob trava e
    // desenhar fora dela: o desenho é longo e a trava é disputada com a thread
    // que decodifica.
    struct NaTela {
        std::string id;
        std::string dono;
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
            aoVivo.push_back({id, quem->dono, paraW(quem->nome), quem->quadro, quem->largura});
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

    // ---- o palco
    const auto palco = telaCheia
                           ? D2D1::RectF(0, 0, larg, alt)
                           : D2D1::RectF(tema::kEspaco, topo, painelX - tema::kEspaco,
                                         alt - tema::kEspaco);
    // Palco mais escuro que o painel, e não igual: a imagem entra com a
    // proporção dela e sobra faixa em volta. Na mesma cor do painel, essa faixa
    // lia como "a interface está quebrada aqui"; escura, lê como moldura.
    render.retangulo(palco, tema::kFundo, tema::kRaioCartao);

    // O que cabe no palco, na ordem em que vai aparecer.
    //
    // A escolhida primeiro, depois as outras, e a própria prévia por último -
    // ver o que os outros mandam é o motivo de estar aqui; a própria tela a
    // gente já tem na frente.
    struct Quadro {
        std::string chave;
        std::wstring nome;
        ID3D11Texture2D* imagem;
        bool minha;
        bool camera;
    };
    std::vector<Quadro> quadros;
    if (escolhida) {
        quadros.push_back({escolhida->id, escolhida->nome, escolhida->quadro, false, false});
    }
    for (const auto& n : aoVivo) {
        if (escolhida && n.id == escolhida->id) continue;
        quadros.push_back({n.id, n.nome, n.quadro, false, false});
    }
    // A própria imagem entra no palco SÓ transmitindo.
    //
    // Antes ela aparecia assim que uma tela estava escolhida, mesmo fora do ar.
    // O palco passa a ideia de "isto está indo para a sala", e mostrar ali algo
    // que não está indo é mentira - a pessoa vê a própria tela no palco e
    // conclui que já está compartilhando. Para se enquadrar antes existe a
    // prévia dentro do modal, que é onde a escolha acontece.
    if (transmitindo) {
        if (telaLigada(monitorEscolhido)) {
            quadros.push_back({"previa", L"Você", previaDaTela(), true, false});
        }
        for (auto& c : camerasAbertas) {
            ID3D11Texture2D* imagem = c->previa(tela.dispositivo(), tela.contexto());
            if (imagem) quadros.push_back({"cam:" + c->id, paraW(c->nome), imagem, true, true});
        }
    }

    const size_t vagas = static_cast<size_t>(divisoes);
    if (quadros.size() > vagas) quadros.resize(vagas);

    btQuadros.clear();
    chavesQuadros.clear();

    if (quadros.empty()) {
        // ---- estado vazio
        const float meio = (palco.top + palco.bottom) / 2.0f;
        const float meioX = (palco.left + palco.right) / 2.0f;

        // Um monitor desenhado, como o Electron faz - e não a logo. A logo diz
        // "este é o GreenLabs", que a pessoa já sabe; o monitor diz do que se
        // está falando.
        icone::monitor(render, D2D1::RectF(meioX - 22, meio - 74, meioX + 22, meio - 30),
                       tema::kLinhaForte, 40.0f, 2.2f);

        const wchar_t* titulo;
        const wchar_t* dica;
        // A placa de vídeo vem antes de tudo. Sem ela não há transmissão de
        // tela possível, e mandar "clique em transmitir" nessa situação é
        // mandar a pessoa clicar num botão que não vai fazer nada - foi
        // exatamente assim que isto apareceu, com alguém clicando e o
        // aplicativo "procurando imagem" para sempre, sem dizer o motivo.
        if (tela.semGPU()) {
            titulo = L"O driver de vídeo caiu";
            dica = L"Sem placa de vídeo não dá para transmitir a tela. "
                   L"Reinicie o computador para voltar ao normal — assistir e "
                   L"a câmera continuam funcionando.";
        } else if (!conectado.load()) {
            titulo = L"Você não está numa sala";
            dica = L"Entre numa sala para ver e ser visto.";
        } else {
            titulo = L"Nenhuma transmissão ativa";
            dica = L"Clique em transmitir para começar";
        }
        render.texto(titulo, D2D1::RectF(palco.left, meio - 14, palco.right, meio + 14),
                     tema::kTexto, Fonte::Subtitulo, DWRITE_TEXT_ALIGNMENT_CENTER);
        render.texto(dica, D2D1::RectF(palco.left, meio + 18, palco.right, meio + 42),
                     tema::kApagado, Fonte::Pequena, DWRITE_TEXT_ALIGNMENT_CENTER);
        render.contorno(palco, tema::kLinha, tema::kRaioCartao);
    } else {
        // ---- a grade
        //
        // As vagas que sobram ficam desenhadas e vazias, de propósito: sem
        // elas, a única transmissão pularia de tamanho a cada pessoa que entra.
        const int colunas = (divisoes == 1) ? 1 : 2;
        const int linhas = (divisoes == 4) ? 2 : 1;
        const float vao = 8;
        const float largCela = (palco.right - palco.left - vao * (colunas + 1)) / colunas;
        const float altCela = (palco.bottom - palco.top - vao * (linhas + 1)) / linhas;

        for (int i = 0; i < divisoes; ++i) {
            const int coluna = i % colunas;
            const int linha = i / colunas;
            const auto cela = D2D1::RectF(
                palco.left + vao + static_cast<float>(coluna) * (largCela + vao),
                palco.top + vao + static_cast<float>(linha) * (altCela + vao),
                palco.left + vao + static_cast<float>(coluna) * (largCela + vao) + largCela,
                palco.top + vao + static_cast<float>(linha) * (altCela + vao) + altCela);

            if (static_cast<size_t>(i) >= quadros.size()) {
                // Vaga livre.
                render.contorno(cela, tema::kLinha, 12);
                const float meioX = (cela.left + cela.right) / 2;
                const float meioY = (cela.top + cela.bottom) / 2;
                icone::monitor(render, D2D1::RectF(meioX - 14, meioY - 30, meioX + 14, meioY - 2),
                               tema::kLinha, 22.0f, 1.6f);
                render.texto(L"vaga livre",
                             D2D1::RectF(cela.left, meioY + 6, cela.right, meioY + 26),
                             tema::kLinhaForte, Fonte::Pequena, DWRITE_TEXT_ALIGNMENT_CENTER);
                continue;
            }

            const Quadro& q = quadros[static_cast<size_t>(i)];
            const bool ativa = q.chave == noPalco;
            const bool sob = apontando(cela);

            render.video(q.chave, q.imagem, cela);
            if (ativa && divisoes > 1) render.contorno(cela, tema::kVerdeLinha, 12, 2.0f);
            else if (sob) render.contorno(cela, tema::kLinhaForte, 12);

            // Rodapé do quadro: pastilha com o ícone da fonte e o nome, sobre a
            // imagem. Ancorado no VÍDEO e não na célula - a imagem entra com a
            // proporção dela e quase nunca preenche a célula inteira, e uma
            // etiqueta no meio da faixa preta fica solta, longe do que descreve.
            const D2D1_RECT_F v = render.areaDoVideo(q.chave);
            const bool temImagem = render.temQuadro(q.chave) && v.right > v.left;
            const D2D1_RECT_F base = temImagem ? v : cela;

            std::wstring rotulo = q.nome;
            if (q.minha) rotulo += L" · Você";

            const float largura = render.larguraDoTexto(rotulo, Fonte::Pequena) + 44;
            const auto etiqueta = D2D1::RectF(base.left + 10, base.bottom - 40,
                                              (std::min)(base.left + 10 + largura, base.right - 10),
                                              base.bottom - 10);
            render.retangulo(etiqueta, tema::kFundo, 15);
            if (ativa) render.contorno(etiqueta, tema::kVerdeLinha, 15);

            const auto areaIcone =
                D2D1::RectF(etiqueta.left + 10, etiqueta.top, etiqueta.left + 26, etiqueta.bottom);
            if (q.camera) icone::camera(render, areaIcone, tema::kVerde, 13.0f);
            else if (q.minha) icone::monitor(render, areaIcone, tema::kApagado, 13.0f);
            else icone::aoVivo(render, areaIcone, tema::kVerde);

            render.texto(rotulo,
                         D2D1::RectF(etiqueta.left + 30, etiqueta.top, etiqueta.right - 6,
                                     etiqueta.bottom),
                         ativa ? tema::kVerde : tema::kTexto, Fonte::Pequena);

            // Botão de expandir, no canto de cima à direita da imagem. Só
            // aparece sob o ponteiro: um botão fixo sobre cada quadro é ruído
            // permanente numa tela cujo conteúdo é a imagem.
            if (sob) {
                const auto bt = D2D1::RectF(base.right - 42, base.top + 10, base.right - 10,
                                            base.top + 42);
                render.retangulo(bt, tema::kFundo, 9);
                render.contorno(bt, tema::kLinha, 9);
                if (telaCheia) icone::encolher(render, bt, tema::kTexto);
                else icone::expandir(render, bt, tema::kTexto);
                btExpandirQuadro = bt;
            }

            btQuadros.push_back(cela);
            chavesQuadros.push_back(q.chave);
        }
    }


    // Em tela cheia acaba aqui. A saída fica escrita num canto, discreta:
    // janela sem borda e sem botão é onde a pessoa se sente presa, e uma linha
    // de texto resolve isso sem pôr moldura de volta na imagem.
    if (telaCheia) {
        render.texto(L"Esc  ou  F11  para sair da tela cheia",
                     D2D1::RectF(larg - 320, alt - 40, larg - 20, alt - 18), tema::kApagado,
                     Fonte::Pequena, DWRITE_TEXT_ALIGNMENT_TRAILING);
        return;
    }

    // ---- painel lateral
    const auto painel = D2D1::RectF(painelX, topo, larg - tema::kEspaco, alt - tema::kEspaco);
    render.retangulo(painel, tema::kPainel, tema::kRaioCartao);
    render.contorno(painel, tema::kLinha, tema::kRaioCartao);

    const float esq = painel.left + 16;
    const float dir = painel.right - 16;

    // A área que rola vai do topo do painel até onde os botões começam. Eles
    // ficam ancorados na base e fora do recorte: botão que rola para fora da
    // vista é botão que não existe na hora em que se precisa dele.
    // Cabeçalho do painel, fora do que rola - como o .side-header do Electron.
    render.texto(L"PAINEL DE CONTROLE",
                 D2D1::RectF(esq, painel.top + 14, dir, painel.top + 32), tema::kVerde,
                 Fonte::Pequena);

    // Sem botoes no pe, a lista vai ate a base do painel - so o diagnostico,
    // que aparece transmitindo, reserva espaco.
    const float alturaDiagnostico = transmitindo ? 74.0f : 0.0f;
    const float topoRolavel = painel.top + 42;
    const float fimRolavel = painel.bottom - 16 - alturaDiagnostico;

    areaRolavel = D2D1::RectF(painel.left, topoRolavel, painel.right, fimRolavel);
    alturaVisivel = fimRolavel - topoRolavel;

    // Limita a rolagem ao que o conteúdo do quadro ANTERIOR pedia. Medir e
    // limitar no mesmo quadro seria tarde: o desenho já teria acontecido com o
    // valor errado, e a lista daria um solavanco visível a cada roda.
    const float limiteRolagem = alturaConteudo > alturaVisivel
                                    ? alturaConteudo - alturaVisivel
                                    : 0.0f;
    if (rolagem > limiteRolagem) rolagem = limiteRolagem;

    render.recortar(areaRolavel);
    float y = topoRolavel - rolagem;
    // Ícones desenhados à mão.
    //
    // O Electron usa lucide; aqui não há biblioteca de ícones nem fonte que se
    // possa contar com um glifo de monitor. Dois retângulos e dois círculos
    // dizem a mesma coisa e não dependem de nada estar instalado.
    auto iconeMonitor = [&](float ix, float iy, const D2D1_COLOR_F& cor) {
        render.contorno(D2D1::RectF(ix, iy, ix + 14, iy + 10), cor, 2);
        render.retangulo(D2D1::RectF(ix + 5, iy + 11, ix + 9, iy + 12), cor, 1);
    };
    auto iconePessoas = [&](float ix, float iy, const D2D1_COLOR_F& cor) {
        render.contorno(D2D1::RectF(ix, iy + 1, ix + 7, iy + 8), cor, 4);
        render.contorno(D2D1::RectF(ix + 6, iy + 3, ix + 12, iy + 9), cor, 3);
    };

    // Uma seção no formato do Electron: barra de título verde com contador em
    // cima e o corpo com borda em volta. A borda é desenhada DEPOIS do conteúdo
    // porque só aí se sabe onde ela termina.
    auto abrirSecao = [&](int qualIcone, const std::wstring& titulo) {
        const float topo = y;
        const auto barra = D2D1::RectF(esq, y, dir, y + 34);
        render.retangulo(barra, tema::kPainel2, 11);
        // O canto de baixo da barra é reto: ela encosta no corpo da seção.
        render.retangulo(D2D1::RectF(barra.left, barra.bottom - 11, barra.right, barra.bottom),
                         tema::kPainel2);
        render.linha(barra.left, barra.bottom, barra.right, barra.bottom, tema::kLinha);

        if (qualIcone == 0) iconeMonitor(barra.left + 12, barra.top + 11, tema::kVerde);
        else iconePessoas(barra.left + 12, barra.top + 12, tema::kVerde);

        render.texto(titulo, D2D1::RectF(barra.left + 34, barra.top, barra.right - 12, barra.bottom),
                     tema::kVerde, Fonte::Pequena);
        y += 34;
        return topo;
    };

    auto fecharSecao = [&](float topo) {
        y += 10;
        render.contorno(D2D1::RectF(esq, topo, dir, y), tema::kLinha, 11);
        y += 12;
    };

    auto vazio = [&](const wchar_t* texto) {
        render.texto(texto, D2D1::RectF(esq + 12, y + 8, dir - 12, y + 46), tema::kApagado,
                     Fonte::Pequena, DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 46;
    };

    // ---- o que está indo no ar
    //
    // Uma linha só, e um botão para mudar. A escolha de telas e câmeras mudou
    // de lugar: era uma pilha de listas aqui na lateral e virou o modal, que é
    // onde ela cabe - com miniatura ao vivo de cada tela, em vez de "Monitor 1"
    // e "Monitor 2" para adivinhar.
    {
        std::wstring resumo;
        if (!telasLigadas.empty()) {
            resumo = std::to_wstring(telasLigadas.size()) +
                     (telasLigadas.size() == 1 ? L" tela" : L" telas");
        }
        const size_t vivas = camerasVivas();
        if (vivas > 0) {
            if (!resumo.empty()) resumo += L" e ";
            resumo += std::to_wstring(vivas) + (vivas == 1 ? L" câmera" : L" câmeras");
        }
        if (resumo.empty()) resumo = L"nada escolhido";

        const auto area = D2D1::RectF(esq, y, dir, y + 42);
        const bool sob = sobre(area) && sobre(areaRolavel);
        if (sob) sobreClicavel = true;
        render.retangulo(area, sob ? tema::kPainel3 : tema::kPainel2, 11);
        render.contorno(area, sob ? tema::kLinhaForte : tema::kLinha, 11);
        iconeMonitor(area.left + 12, area.top + 15, tema::kApagado);
        render.texto(resumo, D2D1::RectF(area.left + 34, area.top, area.right - 66, area.bottom),
                     tema::kTexto, Fonte::Pequena);
        render.texto(L"mudar", D2D1::RectF(area.left, area.top, area.right - 12, area.bottom),
                     tema::kVerde, Fonte::Pequena, DWRITE_TEXT_ALIGNMENT_TRAILING);
        btEscolher = area;
        y += 54;
    }

    // ---- transmissões, com miniatura ao vivo de cada uma
    {
        const float topo =
            abrirSecao(0, L"TRANSMISSÕES (" + std::to_wstring(aoVivo.size()) + L")");

        btTransmissoes.clear();
        idsTransmissoes.clear();

        if (aoVivo.empty()) {
            vazio(L"Nenhuma transmissão ativa no momento.");
        } else {
            y += 10;
            for (const auto& n : aoVivo) {
                const float larguraCartao = dir - esq - 20;
                const float alturaMini = larguraCartao * 9.0f / 16.0f;
                const auto cartao =
                    D2D1::RectF(esq + 10, y, dir - 10, y + alturaMini + 28);

                const bool ativo = n.id == noPalco;
                const bool sob = !ativo && sobre(cartao) && sobre(areaRolavel);
                if (sob) sobreClicavel = true;

                render.retangulo(cartao, ativo ? tema::kVerdeSuave
                                               : (sob ? tema::kPainel3 : tema::kPainel2),
                                 11);
                render.contorno(cartao, ativo ? tema::kVerdeLinha : tema::kLinha, 11);

                const auto areaMini = D2D1::RectF(cartao.left + 5, cartao.top + 5,
                                                  cartao.right - 5, cartao.top + alturaMini);
                render.retangulo(areaMini, tema::kFundo, 8);

                // Quem está no palco NÃO é desenhado de novo aqui: o
                // renderizador guarda, por chave, onde a imagem caiu, e é disso
                // que a etiqueta em cima do vídeo se serve. Desenhar a mesma
                // chave duas vezes mandaria a etiqueta para cima do cartão.
                if (ativo) {
                    render.texto(L"no palco", areaMini, tema::kVerde, Fonte::Pequena,
                                 DWRITE_TEXT_ALIGNMENT_CENTER);
                } else {
                    render.video(n.id, n.quadro, areaMini);
                }

                const float recuoNome = ativo ? 26.0f : 11.0f;
                if (ativo) {
                    icone::aoVivo(render,
                                  D2D1::RectF(cartao.left + 10, areaMini.bottom + 1,
                                              cartao.left + 22, cartao.bottom - 3),
                                  tema::kVerde);
                }
                render.texto(n.nome,
                             D2D1::RectF(cartao.left + recuoNome, areaMini.bottom + 1,
                                         cartao.right - 11, cartao.bottom - 3),
                             ativo ? tema::kVerde : tema::kTexto, Fonte::Pequena);

                btTransmissoes.push_back(cartao);
                idsTransmissoes.push_back(n.id);
                y += alturaMini + 38;
            }
            y -= 10;
        }
        fecharSecao(topo);
    }

    // ---- quem está na sala
    std::vector<Participante> copia;
    {
        std::lock_guard trava(travaPares);
        copia = pares;
    }

    {
        const float topo =
            abrirSecao(1, L"PESSOAS (" + std::to_wstring(copia.size() + 1) + L")");
        y += 10;

        // Uma pessoa por linha, com avatar de iniciais e pastilha de ping - o
        // mesmo desenho do .user-row-card do Electron. O ponto verde diz quem
        // está no palco: o palco troca sozinho quando alguém transmite, e nada
        // dizia que aquilo tinha acontecido nem de quem era a tela.
        auto pessoa = [&](const std::wstring& nome, int ping, bool souEu, bool noPalcoAgora) {
            const auto area = D2D1::RectF(esq + 10, y, dir - 10, y + 44);
            render.retangulo(area, noPalcoAgora ? tema::kVerdeSuave : tema::kPainel2, 12);
            render.contorno(area, noPalcoAgora ? tema::kVerdeLinha : tema::kLinha, 12);

            // Avatar: as duas primeiras letras, em maiúsculas.
            std::wstring iniciais = nome.substr(0, 2);
            for (auto& c : iniciais) c = static_cast<wchar_t>(::towupper(c));
            const auto circulo =
                D2D1::RectF(area.left + 8, area.top + 7, area.left + 38, area.top + 37);
            render.retangulo(circulo, tema::kVerdeSuave, 15);
            render.contorno(circulo, tema::kVerdeLinha, 15);
            render.texto(iniciais, circulo, tema::kTexto, Fonte::Pequena,
                         DWRITE_TEXT_ALIGNMENT_CENTER);

            const float direitaNome = ping > 0 ? area.right - 66 : area.right - 12;
            render.texto(nome, D2D1::RectF(area.left + 46, area.top, direitaNome, area.bottom),
                         tema::kTexto, Fonte::Pequena);
            if (souEu) {
                const float largura = render.larguraDoTexto(nome, Fonte::Pequena);
                render.texto(L"(você)",
                             D2D1::RectF(area.left + 52 + largura, area.top, direitaNome,
                                         area.bottom),
                             tema::kVerde, Fonte::Pequena);
            }

            if (ping > 0) {
                const std::wstring texto = std::to_wstring(ping) + L"ms";
                const float largura = render.larguraDoTexto(texto, Fonte::Pequena) + 14;
                const auto pastilha = D2D1::RectF(area.right - 10 - largura, area.top + 12,
                                                  area.right - 10, area.top + 32);
                render.retangulo(pastilha, tema::kVerdeSuave, 6);
                render.contorno(pastilha, tema::kVerdeLinha, 6);
                render.texto(texto, pastilha, tema::kVerde, Fonte::Pequena,
                             DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            y += 50;
        };

        const std::wstring meuNome = campoNome.valor.empty() ? L"Você" : campoNome.valor;
        pessoa(meuNome, static_cast<int>(sinal.pingMs()), true, transmitindo && !escolhida);

        for (const auto& p : copia) {
            const bool estaNoPalco =
                escolhida && !escolhida->dono.empty() && escolhida->dono == p.id;
            pessoa(paraW(p.nome), p.pingMs, false, estaNoPalco);
        }

        y -= 6;
        fecharSecao(topo);
    }

    // ---- volume do que CHEGA da chamada
    //
    // Fica por último porque é ajuste, não escolha: quem abre o painel está
    // procurando quem está na sala, não o volume.
    {
        render.texto(L"VOLUME DE QUEM EU OUÇO", D2D1::RectF(esq, y, dir - 44, y + 14),
                     tema::kApagado, Fonte::Pequena);
        render.texto(std::to_wstring(volumeDaChamada) + L"%",
                     D2D1::RectF(esq, y, dir, y + 14), tema::kTexto, Fonte::Pequena,
                     DWRITE_TEXT_ALIGNMENT_TRAILING);
        y += 22;

        barraVolume = D2D1::RectF(esq, y, dir, y + 18);
        const float trilhoY = y + 7;
        render.retangulo(D2D1::RectF(esq, trilhoY, dir, trilhoY + 5), tema::kLinha, 3);

        const float fracao = static_cast<float>(volumeDaChamada) / 100.0f;
        const float ate = esq + (dir - esq) * fracao;
        if (ate > esq) {
            render.retangulo(D2D1::RectF(esq, trilhoY, ate, trilhoY + 5), tema::kVerde, 3);
        }

        // A bolinha fica presa dentro da barra nas pontas, senão ela some
        // metade para fora no 0 e no 100.
        const float centro = (ate < esq + 7) ? esq + 7 : ((ate > dir - 7) ? dir - 7 : ate);
        render.retangulo(D2D1::RectF(centro - 7, trilhoY - 4, centro + 7, trilhoY + 9),
                         tema::kTexto, 7);
        y += 26;
    }

    // Fim da parte que rola. A altura medida aqui é o que limita a rolagem no
    // quadro seguinte.
    alturaConteudo = (y + rolagem) - topoRolavel;
    render.soltarRecorte();

    // Trilho de rolagem, e só quando há o que rolar.
    //
    // Fininho e encostado na borda: ele existe para dizer "há mais coisa aqui
    // embaixo", não para ser agarrado - quem rola usa a roda.
    if (alturaConteudo > alturaVisivel) {
        const float fracaoVisivel = alturaVisivel / alturaConteudo;
        const float alturaPolegar = alturaVisivel * fracaoVisivel;
        const float andado = (limiteRolagem > 0) ? (rolagem / limiteRolagem) : 0.0f;
        const float y0 = topoRolavel + (alturaVisivel - alturaPolegar) * andado;
        const float x0 = painel.right - 6;
        render.retangulo(D2D1::RectF(x0, y0, x0 + 3, y0 + alturaPolegar), tema::kLinha, 2);
    }

    // ---- pé do painel: só o diagnóstico
    //
    // Os botões SAIR e TRANSMITIR viviam aqui E na barra de cima. Dois botões
    // que fazem a mesma coisa, na mesma tela, obrigam a pessoa a descobrir se
    // são o mesmo - e duas portas lado a lado é pergunta, não interface. Ficou
    // a barra, que é onde o Electron os põe.
    if (!transmitindo) return;
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


// ---------------------------------------------------------------- o modal

void Aplicacao::Interno::abrirModal() {
    modal = Modal::Fonte;
    abaModal = 0;
    rolagemModal = 0;
    alturaConteudoModal = 0;
    telasNoModal = telasLigadas;
    camerasNoModal = camerasEscolhidas;
    audioNoModal = audioLigado;
    qualidadeNoModal = qualidadeEscolhida;

    // Miniatura ao vivo de cada monitor.
    //
    // Duplicar um monitor não acende nada nem aparece para ninguém - dá para
    // abrir todas enquanto o modal está aberto e ver de verdade o que há em
    // cada tela antes de escolher. É o que a janela do Electron faz, e sem isso
    // a escolha vira adivinhação entre "Monitor 1" e "Monitor 2".
    previasDoModal.clear();
    quadrosDoModal.clear();
    previasDoModal.resize(monitores.size());
    quadrosDoModal.resize(monitores.size());
    for (size_t i = 0; i < monitores.size(); ++i) {
        if (static_cast<int>(i) == monitorEscolhido) continue;  // esse já é o `tela`
        auto captura = std::make_unique<ScreenCapture>();
        if (captura->iniciarCom(tela.dispositivo(), tela.contexto(), static_cast<uint32_t>(i))) {
            previasDoModal[i] = std::move(captura);
        }
    }
}

void Aplicacao::Interno::fecharModal() {
    modal = Modal::Nenhum;
    for (auto& p : previasDoModal) {
        if (p) p->parar();
    }
    previasDoModal.clear();
    quadrosDoModal.clear();
    for (size_t i = 0; i < monitores.size(); ++i) {
        render.esquecerVideo("modal:" + std::to_string(i));
    }
}

void Aplicacao::Interno::confirmarModal() {
    telasLigadas = telasNoModal;
    std::sort(telasLigadas.begin(), telasLigadas.end());
    qualidadeEscolhida = qualidadeNoModal;
    audioLigado = audioNoModal;

    config.telas = telasLigadas;
    config.qualidade = qualidadeEscolhida;
    config.audio = audioLigado;

    // O dispositivo D3D vem de um monitor, mesmo quando ele não vai no ar. Só
    // precisa trocar quando o que ele usa deixou de existir na lista - e essa é
    // a operação cara, que refaz encoder, decodificador e interface.
    const bool precisaTrocarDono = !telasLigadas.empty() && !telaLigada(monitorEscolhido);
    const int novoDono = telasLigadas.empty() ? monitorEscolhido : telasLigadas.front();

    fecharModal();

    // As câmeras primeiro: aplicarCameras já salva e refaz o encode, e na ordem
    // contrária o encode seria refeito duas vezes.
    aplicarCameras(camerasNoModal);

    if (precisaTrocarDono) {
        trocarPrincipal(novoDono);
    } else if (transmitindo) {
        reiniciarEncode();
    } else {
        config.salvar();
    }

    // Confirmar com algo marcado e fora do ar é o mesmo que apertar
    // TRANSMITIR - é para isso que a pessoa abriu esta janela.
    if (!transmitindo && (!telasLigadas.empty() || !camerasEscolhidas.empty())) {
        comecarTransmissao();
    }
}

// Mantém as miniaturas do modal vivas: uma cópia por monitor por quadro, e só
// enquanto ele está aberto.
void Aplicacao::Interno::bombearPreviasDoModal() {
    for (size_t i = 0; i < previasDoModal.size(); ++i) {
        auto& captura = previasDoModal[i];
        if (!captura) continue;

        QuadroCapturado quadro;
        const auto estado = captura->proximoQuadro(0, quadro);
        if (estado == ResultadoQuadro::PrecisaReiniciar) {
            captura->reiniciar();
            continue;
        }
        if (estado != ResultadoQuadro::Ok || !quadro.textura) continue;

        D3D11_TEXTURE2D_DESC origem{};
        quadro.textura->GetDesc(&origem);
        if (quadrosDoModal[i]) {
            D3D11_TEXTURE2D_DESC atual{};
            quadrosDoModal[i]->GetDesc(&atual);
            if (atual.Width != origem.Width || atual.Height != origem.Height) {
                quadrosDoModal[i].Reset();
            }
        }
        if (!quadrosDoModal[i]) {
            D3D11_TEXTURE2D_DESC nova = origem;
            nova.Usage = D3D11_USAGE_DEFAULT;
            nova.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            nova.CPUAccessFlags = 0;
            nova.MiscFlags = 0;
            nova.MipLevels = 1;
            nova.ArraySize = 1;
            tela.dispositivo()->CreateTexture2D(&nova, nullptr, &quadrosDoModal[i]);
        }
        if (quadrosDoModal[i]) {
            tela.contexto()->CopyResource(quadrosDoModal[i].Get(), quadro.textura);
        }
        captura->liberarQuadro();
    }
}

// A janela de escolher o que transmitir.
//
// Medidas e cores saíram do .picker-modal do cliente em Electron, uma a uma:
// largura de 540, cantos de 24, cabeçalho e abas separados por linha, corpo em
// grade de dois, interruptor de som no pé e um botão verde de largura cheia.
void Aplicacao::Interno::desenharModal() {
    // O pé tem altura fixa porque o conteúdo dele é fixo: rótulo, pastilhas de
    // qualidade, interruptor de som e o botão. Ele estava em 152 e o botão caía
    // POR CIMA do interruptor - dá para ver na janela: "MARQUE PELO MENOS UM"
    // escrito em cima de "Mandar o som do computador junto".
    //
    //   14 rótulo + 20 até as pastilhas + 30 pastilhas
    // + 42 respiro + 44 interruptor
    // + 18 respiro + 42 botão + 18 margem
    const float alturaPe = 198;

    std::vector<D2D1_RECT_F> areasDasAbas;
    const auto moldura = desenharMoldura(
        0, L"Escolha o que transmitir", L"Suas telas e suas câmeras. Dá para marcar mais de uma.",
        alturaPe, {L"Telas (" + std::to_wstring(monitores.size()) + L")",
                   L"Câmeras (" + std::to_wstring(cameras.size()) + L")"},
        abaModal, areasDasAbas);

    btAbaTelas = areasDasAbas[0];
    btAbaCameras = areasDasAbas[1];

    const float x = moldura.cartao.left;
    const float largModal = moldura.cartao.right - moldura.cartao.left;

    // ---- pé
    const float basePe = moldura.basePe;
    const float topoPe = basePe - alturaPe;
    render.linha(x, topoPe, x + largModal, topoPe, tema::kLinha);

    float qy = topoPe + 14;
    render.texto(L"QUALIDADE", D2D1::RectF(x + 20, qy, x + largModal - 20, qy + 14), tema::kApagado,
                 Fonte::Pequena);
    qy += 20;
    btQualidades.clear();
    float qx = x + 20;
    for (size_t i = 0; i < std::size(kQualidades); ++i) {
        const std::wstring rotulo = kQualidades[i].rotulo;
        const float largura = render.larguraDoTexto(rotulo, Fonte::Pequena) + 26;
        const auto area = D2D1::RectF(qx, qy, qx + largura, qy + 30);
        const bool ativa = static_cast<int>(i) == qualidadeNoModal;
        const bool sob = !ativa && apontando(area);
        render.retangulo(area, ativa ? tema::kVerdeSuave : (sob ? tema::kPainel3 : tema::kPainel2),
                         9);
        if (ativa) render.contorno(area, tema::kVerdeLinha, 9);
        render.texto(rotulo, area, ativa ? tema::kVerde : tema::kTexto, Fonte::Pequena,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        btQualidades.push_back(area);
        qx += largura + 8;
    }

    // Som do sistema, no mesmo desenho do .check-row do Electron.
    const float ay = qy + 42;
    btModalAudio = D2D1::RectF(x + 20, ay, x + largModal - 20, ay + 44);
    render.retangulo(btModalAudio, apontando(btModalAudio) ? tema::kPainel3 : tema::kPainel2, 12);
    render.contorno(btModalAudio, tema::kLinha, 12);
    {
        const float tl = btModalAudio.left + 14;
        const float tt = btModalAudio.top + 10;
        render.retangulo(D2D1::RectF(tl, tt, tl + 44, tt + 24),
                         audioNoModal ? tema::kVerde : tema::kLinha, 12);
        const float bola = audioNoModal ? tl + 23 : tl + 3;
        render.retangulo(D2D1::RectF(bola, tt + 3, bola + 18, tt + 21), tema::kTexto, 9);
    }
    render.texto(L"Mandar o som do computador junto",
                 D2D1::RectF(btModalAudio.left + 74, btModalAudio.top, btModalAudio.right - 14,
                             btModalAudio.bottom),
                 tema::kTexto, Fonte::Pequena);

    const int marcadas = static_cast<int>(telasNoModal.size() + camerasNoModal.size());
    // Ancorado ABAIXO do interruptor, e não medido de baixo para cima: era daí
    // que vinha a sobreposição.
    btModalConfirmar =
        D2D1::RectF(x + 20, btModalAudio.bottom + 18, x + largModal - 20, btModalAudio.bottom + 60);
    const bool podeConfirmar = marcadas > 0;
    render.retangulo(btModalConfirmar,
                     podeConfirmar
                         ? (apontando(btModalConfirmar) ? tema::kVerde : tema::kVerdeForte)
                         : tema::kPainel2,
                     11);

    std::wstring rotuloConfirmar = L"MARQUE PELO MENOS UM";
    if (marcadas == 1) rotuloConfirmar = L"TRANSMITIR";
    else if (marcadas > 1) rotuloConfirmar = L"TRANSMITIR OS " + std::to_wstring(marcadas);

    {
        const auto cor = podeConfirmar ? tema::kFundo : tema::kApagado;
        const float largura = render.larguraDoTexto(rotuloConfirmar, Fonte::Botao);
        const float meio = (btModalConfirmar.left + btModalConfirmar.right) / 2;
        const float inicio = meio - (largura + (podeConfirmar ? 26.0f : 0.0f)) / 2;
        if (podeConfirmar) {
            icone::transmitir(render,
                              D2D1::RectF(inicio, btModalConfirmar.top, inicio + 18,
                                          btModalConfirmar.bottom),
                              cor, 16.0f, 1.7f);
        }
        render.texto(rotuloConfirmar,
                     D2D1::RectF(inicio + (podeConfirmar ? 26.0f : 0.0f), btModalConfirmar.top,
                                 btModalConfirmar.right, btModalConfirmar.bottom),
                     cor, Fonte::Botao);
    }

    // ---- corpo: a grade de cartões
    const auto corpo = moldura.corpo;

    // A grade rola: numa janela baixa, ou com muitas câmeras, ela não cabe. O
    // limite vem da medida do quadro anterior, pelo mesmo motivo do painel -
    // medir e limitar no mesmo quadro daria um solavanco a cada roda.
    areaRolavelModal = corpo;
    const float visivelModal = corpo.bottom - corpo.top;
    const float limiteModal =
        alturaConteudoModal > visivelModal ? alturaConteudoModal - visivelModal : 0.0f;
    if (rolagemModal > limiteModal) rolagemModal = limiteModal;

    render.recortar(corpo);

    btCartoesModal.clear();
    const float largCartao = (corpo.right - corpo.left - 12) / 2;
    const float altMini = largCartao * 9.0f / 16.0f;
    const float altCartao = altMini + 38;

    int coluna = 0;
    float cy = corpo.top - rolagemModal;

    auto cartao = [&](const std::wstring& nome, const std::wstring& detalhe, bool marcado,
                      const std::string& chave, ID3D11Texture2D* imagem) {
        const float cx = corpo.left + coluna * (largCartao + 12);
        const auto area = D2D1::RectF(cx, cy, cx + largCartao, cy + altCartao);
        const bool sob = apontando(area);

        render.retangulo(area, tema::kPainel2, 14);
        render.contorno(area,
                        marcado ? tema::kVerdeLinha : (sob ? tema::kLinhaForte : tema::kLinha), 14);

        const auto areaMini =
            D2D1::RectF(area.left + 8, area.top + 8, area.right - 8, area.top + 8 + altMini);
        render.retangulo(areaMini, tema::kFundo, 8);
        if (imagem || render.temQuadro(chave)) {
            render.video(chave, imagem, areaMini);
        } else {
            render.texto(L"sem imagem ainda", areaMini, tema::kApagado, Fonte::Pequena,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        // A marca de escolhido fica SOBRE a miniatura: é da imagem que se está
        // falando, e no canto do cartão ela competiria com o nome.
        const auto marca =
            D2D1::RectF(areaMini.right - 28, areaMini.top + 6, areaMini.right - 6, areaMini.top + 28);
        render.retangulo(marca, marcado ? tema::kVerde : tema::kSombraModal, 11);
        if (marcado) {
            icone::visto(render, marca, tema::kFundo);
        } else {
            render.contorno(marca, tema::kLinhaForte, 11);
        }

        render.texto(nome,
                     D2D1::RectF(area.left + 10, areaMini.bottom + 3, area.right - 10,
                                 areaMini.bottom + 21),
                     marcado ? tema::kVerde : tema::kTexto, Fonte::Pequena);
        render.texto(detalhe,
                     D2D1::RectF(area.left + 10, areaMini.bottom + 19, area.right - 10,
                                 area.bottom - 2),
                     tema::kApagado, Fonte::Pequena);

        btCartoesModal.push_back(area);
        if (++coluna == 2) {
            coluna = 0;
            cy += altCartao + 12;
        }
    };

    if (abaModal == 0) {
        for (size_t i = 0; i < monitores.size(); ++i) {
            const int indice = static_cast<int>(i);
            const bool marcada =
                std::find(telasNoModal.begin(), telasNoModal.end(), indice) != telasNoModal.end();
            ID3D11Texture2D* imagem =
                (indice == monitorEscolhido)
                    ? previaDaTela()
                    : (quadrosDoModal.size() > i ? quadrosDoModal[i].Get() : nullptr);
            cartao(L"Monitor " + std::to_wstring(i + 1),
                   std::to_wstring(monitores[i].largura) + L"×" +
                       std::to_wstring(monitores[i].altura),
                   marcada, "modal:" + std::to_string(i), imagem);
        }
    } else if (cameras.empty()) {
        render.texto(L"Nenhuma câmera encontrada neste computador.",
                     D2D1::RectF(corpo.left, corpo.top + 20, corpo.right, corpo.top + 44),
                     tema::kApagado, Fonte::Pequena, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        for (const auto& c : cameras) {
            const bool marcada = std::find(camerasNoModal.begin(), camerasNoModal.end(), c.id) !=
                                 camerasNoModal.end();
            CameraAberta* aberta = cameraAberta(c.id);
            ID3D11Texture2D* imagem =
                aberta ? aberta->previa(tela.dispositivo(), tela.contexto()) : nullptr;
            cartao(paraW(c.nome), marcada ? L"ligada" : L"marque para ver", marcada, "cam:" + c.id,
                   imagem);
        }
    }

    // Se a última fileira ficou pela metade, ela ainda ocupa altura.
    if (coluna != 0) cy += altCartao + 12;
    alturaConteudoModal = (cy + rolagemModal) - corpo.top;

    render.soltarRecorte();

    // Trilho de rolagem, fininho e encostado na borda: ele existe para dizer
    // "há mais coisa aqui embaixo", não para ser agarrado.
    if (alturaConteudoModal > visivelModal) {
        const float fracao = visivelModal / alturaConteudoModal;
        const float alturaPolegar = visivelModal * fracao;
        const float andado = (limiteModal > 0) ? (rolagemModal / limiteModal) : 0.0f;
        const float y0 = corpo.top + (visivelModal - alturaPolegar) * andado;
        render.retangulo(D2D1::RectF(corpo.right + 6, y0, corpo.right + 9, y0 + alturaPolegar),
                         tema::kLinhaForte, 2);
    }
}


// A moldura que os dois modais dividem.
//
// Fundo escuro por cima da janela, cartão no meio, cabeçalho com o quadrado do
// ícone, título, subtítulo e o X, e a fileira de abas. O que muda de um modal
// para o outro é só o que vai dentro — e por isso os dois são idênticos por
// construção, e não por eu ter copiado as medidas de um para o outro e
// lembrado de manter as duas cópias iguais.
Aplicacao::Interno::MolduraModal Aplicacao::Interno::desenharMoldura(
    int qualIcone, const std::wstring& titulo, const std::wstring& subtitulo,
    float alturaPe, const std::vector<std::wstring>& abas, int abaAtiva,
    std::vector<D2D1_RECT_F>& areasDasAbas) {
    const float larg = render.largura();
    const float alt = render.altura();

    render.retangulo(D2D1::RectF(0, 0, larg, alt), tema::kSombraModal);

    const float alturaCabecalho = 72;
    const float alturaAbas = abas.empty() ? 0.0f : 42.0f;

    const float largModal = (std::min)(560.0f, larg - 48.0f);
    const float alturaMinima = alturaCabecalho + alturaAbas + alturaPe + 120;
    const float altModal = (std::max)(alturaMinima, (std::min)(alt - 48.0f, 640.0f));
    const float x = (larg - largModal) / 2;
    const float y = (std::max)(8.0f, (alt - altModal) / 2);
    const auto cartao = D2D1::RectF(x, y, x + largModal, y + altModal);

    render.retangulo(cartao, tema::kFundoModal, 24);
    render.contorno(cartao, tema::kLinha, 24);

    render.linha(x, y + alturaCabecalho, x + largModal, y + alturaCabecalho, tema::kLinha);

    const auto quadradoIcone = D2D1::RectF(x + 20, y + 18, x + 56, y + 54);
    render.retangulo(quadradoIcone, tema::kVerdeSuave, 10);
    if (qualIcone == 0) icone::monitor(render, quadradoIcone, tema::kVerde, 17.0f, 1.7f);
    else icone::engrenagem(render, quadradoIcone, tema::kVerde, 18.0f, 1.7f);

    render.texto(titulo, D2D1::RectF(x + 68, y + 17, x + largModal - 62, y + 39), tema::kTexto,
                 Fonte::Subtitulo);
    render.texto(subtitulo, D2D1::RectF(x + 68, y + 39, x + largModal - 62, y + 57),
                 tema::kApagado, Fonte::Pequena);

    btModalFechar = D2D1::RectF(x + largModal - 52, y + 18, x + largModal - 16, y + 54);
    render.retangulo(btModalFechar, apontando(btModalFechar) ? tema::kPainel3 : tema::kPainel2, 12);
    render.contorno(btModalFechar, tema::kLinha, 12);
    icone::fechar(render, btModalFechar, tema::kTexto);

    areasDasAbas.clear();
    const float topoAbas = y + alturaCabecalho;
    if (!abas.empty()) {
        render.linha(x, topoAbas + alturaAbas, x + largModal, topoAbas + alturaAbas, tema::kLinha);
        float esquerda = x + 20;
        for (size_t i = 0; i < abas.size(); ++i) {
            const float largura = render.larguraDoTexto(abas[i], Fonte::Pequena) + 28;
            const auto area =
                D2D1::RectF(esquerda, topoAbas + 4, esquerda + largura, topoAbas + alturaAbas);
            const bool ativa = static_cast<int>(i) == abaAtiva;
            const bool sob = apontando(area);
            render.texto(abas[i], area,
                         ativa ? tema::kVerde : (sob ? tema::kTexto : tema::kApagado),
                         Fonte::Pequena, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (ativa) {
                render.retangulo(D2D1::RectF(area.left, area.bottom - 2, area.right, area.bottom),
                                 tema::kVerde, 1);
            }
            areasDasAbas.push_back(area);
            esquerda = area.right + 8;
        }
    }

    MolduraModal moldura;
    moldura.cartao = cartao;
    moldura.basePe = y + altModal;
    moldura.corpo = D2D1::RectF(x + 20, topoAbas + alturaAbas + 16, x + largModal - 20,
                                moldura.basePe - alturaPe - 14);
    return moldura;
}

void Aplicacao::Interno::abrirConfig() {
    modal = Modal::Config;
    abaConfig = 0;
    campoNome.valor = paraW(config.nome);
    campoServidor.valor = paraW(config.servidor);
    campoSala.valor = paraW(config.sala);
    avisoConfig.clear();
}

// Configuração: conexão e servidores salvos.
//
// Sem a aba Hospedar do Electron. Lá ela sobe o servidor num processo filho do
// Node; aqui não há esse processo, e mostrar um botão que nunca funciona é pior
// do que não mostrar - é a mesma regra que o próprio Electron usa para esconder
// a aba no navegador.
void Aplicacao::Interno::desenharConfig() {
    std::vector<D2D1_RECT_F> areasDasAbas;
    const float alturaPe = 76;
    const auto moldura = desenharMoldura(1, L"Configuração",
                                         L"Conexão e servidores salvos.", alturaPe,
                                         {L"Conexão", L"Servidores"}, abaConfig, areasDasAbas);
    btAbasConfig = areasDasAbas;

    const auto& corpo = moldura.corpo;

    // Pé: um botão só, de largura cheia, como o "Concluir" do Electron.
    btConfigConcluir = D2D1::RectF(corpo.left, moldura.basePe - 60, corpo.right,
                                   moldura.basePe - 18);
    render.linha(moldura.cartao.left, moldura.basePe - alturaPe, moldura.cartao.right,
                 moldura.basePe - alturaPe, tema::kLinha);
    render.retangulo(btConfigConcluir,
                     apontando(btConfigConcluir) ? tema::kVerde : tema::kVerdeForte, 11);
    {
        const float largura = render.larguraDoTexto(L"CONCLUIR", Fonte::Botao);
        const float meio = (btConfigConcluir.left + btConfigConcluir.right) / 2;
        const float inicio = meio - (largura + 24) / 2;
        icone::visto(render,
                     D2D1::RectF(inicio, btConfigConcluir.top, inicio + 16,
                                 btConfigConcluir.bottom),
                     tema::kFundo, 12.0f, 2.2f);
        render.texto(L"CONCLUIR",
                     D2D1::RectF(inicio + 24, btConfigConcluir.top, btConfigConcluir.right,
                                 btConfigConcluir.bottom),
                     tema::kFundo, Fonte::Botao);
    }

    render.recortar(corpo);
    float y = corpo.top;

    auto rotulo = [&](const wchar_t* texto) {
        render.texto(texto, D2D1::RectF(corpo.left, y, corpo.right, y + 14), tema::kApagado,
                     Fonte::Pequena);
        y += 18;
    };

    if (abaConfig == 0) {
        rotulo(L"SEU APELIDO");
        campoNome.area = D2D1::RectF(corpo.left, y, corpo.right, y + 44);
        desenharCampoSimples(campoNome);
        y += 56;

        rotulo(L"SERVIDOR");
        campoServidor.area = D2D1::RectF(corpo.left, y, corpo.right, y + 44);
        desenharCampoSimples(campoServidor);
        y += 56;

        rotulo(L"SALA");
        campoSala.area = D2D1::RectF(corpo.left, y, corpo.right, y + 44);
        desenharCampoSimples(campoSala);
        y += 60;

        // Dois botões lado a lado, como no ConnectionTab do Electron.
        const float meio = (corpo.left + corpo.right) / 2;
        btSalvarPadrao = D2D1::RectF(corpo.left, y, meio - 5, y + 40);
        btRestaurar = D2D1::RectF(meio + 5, y, corpo.right, y + 40);

        render.retangulo(btSalvarPadrao,
                         apontando(btSalvarPadrao) ? tema::kPainel3 : tema::kPainel2, 11);
        render.contorno(btSalvarPadrao, tema::kLinha, 11);
        render.texto(L"Salvar como padrão", btSalvarPadrao, tema::kTexto, Fonte::Pequena,
                     DWRITE_TEXT_ALIGNMENT_CENTER);

        render.retangulo(btRestaurar, apontando(btRestaurar) ? tema::kPainel3 : tema::kPainel2, 11);
        render.contorno(btRestaurar, tema::kLinha, 11);
        render.texto(L"Restaurar de fábrica", btRestaurar, tema::kApagado, Fonte::Pequena,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 52;

        if (!avisoConfig.empty()) {
            render.texto(avisoConfig, D2D1::RectF(corpo.left, y, corpo.right, y + 20),
                         tema::kVerde, Fonte::Pequena, DWRITE_TEXT_ALIGNMENT_CENTER);
            y += 24;
        }

        // O aviso do servidor mora AQUI, e só aqui.
        //
        // Ele ocupava uma faixa larga no pé da janela inteira, no cliente em
        // Electron, enquanto ninguém estivesse conectado - bem na hora em que a
        // pessoa está olhando o palco vazio tentando entender o que fazer. O
        // lugar dele é ao lado do campo do servidor, que é o momento em que se
        // escolhe em quem confiar.
        y += 8;
        render.linha(corpo.left, y, corpo.right, y, tema::kLinha);
        y += 14;
        render.texto(L"ENTRE SÓ EM SERVIDOR DE CONFIANÇA",
                     D2D1::RectF(corpo.left, y, corpo.right, y + 16), tema::kAmarelo,
                     Fonte::Pequena);
        y += 20;
        render.texto(L"Quando vocês não conseguem se conectar direto, sua tela e seu som passam "
                     L"pelo servidor. Quem controla esse servidor consegue gravar o que passa "
                     L"por ele.",
                     D2D1::RectF(corpo.left, y, corpo.right, y + 60), tema::kApagado,
                     Fonte::Pequena);
        y += 64;
    } else {
        btServidoresConfig.clear();
        btRemoverServidor.clear();

        if (config.servidores.empty()) {
            render.texto(L"Nenhum servidor salvo ainda.",
                         D2D1::RectF(corpo.left, y + 20, corpo.right, y + 44), tema::kApagado,
                         Fonte::Pequena, DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            for (const auto& endereco : config.servidores) {
                const auto area = D2D1::RectF(corpo.left, y, corpo.right, y + 46);
                const bool atual = endereco == config.servidor;
                const bool sob = !atual && apontando(area);
                render.retangulo(area, atual ? tema::kVerdeSuave
                                             : (sob ? tema::kPainel3 : tema::kPainel2),
                                 11);
                render.contorno(area, atual ? tema::kVerdeLinha : tema::kLinha, 11);

                render.texto(paraW(endereco),
                             D2D1::RectF(area.left + 14, area.top, area.right - 88, area.bottom),
                             atual ? tema::kVerde : tema::kTexto, Fonte::Pequena);
                render.texto(atual ? L"em uso" : L"usar",
                             D2D1::RectF(area.left, area.top, area.right - 44, area.bottom),
                             atual ? tema::kVerde : tema::kApagado, Fonte::Pequena,
                             DWRITE_TEXT_ALIGNMENT_TRAILING);

                const auto remover =
                    D2D1::RectF(area.right - 36, area.top + 12, area.right - 14, area.top + 34);
                icone::fechar(render, remover,
                              apontando(remover) ? tema::kVermelho : tema::kApagado, 9.0f);

                btServidoresConfig.push_back(area);
                btRemoverServidor.push_back(remover);
                y += 54;
            }
        }
    }

    alturaConteudoConfig = y - corpo.top;
    render.soltarRecorte();
}

void Aplicacao::Interno::desenhar() {
    render.comecarQuadro();
    render.limpar(tema::kFundo);

    // Zerado a cada quadro e remarcado por quem estiver sob o ponteiro. É o que
    // decide o cursor: quem não desenha nada clicável no lugar do mouse deixa
    // a seta como está.
    sobreClicavel = false;

    if (telaAtual == Tela::Entrada) desenharEntrada();
    else desenharAoVivo();

    // A barra some em tela cheia: ela é a única coisa que ainda desenharia por
    // cima da imagem depois que o painel sai.
    if (!telaCheia) {
        desenharBarraTitulo();
        if (telaAtual == Tela::AoVivo) desenharBarraDeAcoes();
    }

    // O modal por último, por cima de tudo - inclusive da barra de título, que
    // é o comportamento de qualquer janela modal.
    if (modal == Modal::Fonte) desenharModal();
    else if (modal == Modal::Config) desenharConfig();
    render.terminarQuadro();
}

}  // namespace gl
