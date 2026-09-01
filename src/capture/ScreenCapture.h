#pragma once

// Captura de tela inteira com DXGI Desktop Duplication.
//
// Só tela inteira, de propósito. A captura de janela isolada (Windows Graphics
// Capture) desenha a borda amarela em volta da janela no Windows 10, e desligar
// isso depende de uma API restrita do Windows 11. A duplicação de área de
// trabalho não desenha nada: nenhuma borda, nenhuma sobreposição, nenhum aviso.
//
// O quadro nunca sai da GPU: o que sai daqui é uma ID3D11Texture2D pronta para
// ir direto ao encoder por hardware. Nenhuma cópia para a CPU acontece neste
// caminho.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "capture/Cursor.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace gl {

struct MonitorInfo {
    uint32_t indice = 0;
    std::string nome;

    // Medidas LÓGICAS: como a área de trabalho aparece para quem olha. Num
    // monitor girado para retrato, são 1080x1920.
    uint32_t largura = 0;
    uint32_t altura = 0;
    bool primario = false;

    // Quantos graus a imagem está girada em relação ao painel.
    //
    // Isto existe porque a duplicação NÃO devolve a textura como a pessoa vê:
    // ela devolve na orientação física do painel. Um monitor 1920x1080 posto em
    // pé continua entregando uma textura 1920x1080, com o conteúdo deitado.
    // Ignorar este campo é o que fazia a tela do monitor em retrato chegar
    // virada do outro lado.
    uint32_t graus = 0;  // 0, 90, 180 ou 270

    // Medidas FÍSICAS: o tamanho real da textura que a duplicação entrega. Com
    // 90 ou 270 graus, são as lógicas trocadas.
    uint32_t larguraFisica() const { return (graus == 90 || graus == 270) ? altura : largura; }
    uint32_t alturaFisica() const { return (graus == 90 || graus == 270) ? largura : altura; }
};

struct QuadroCapturado {
    // Emprestada: vale até a próxima chamada de liberarQuadro(). Não guardar.
    ID3D11Texture2D* textura = nullptr;
    uint32_t largura = 0;
    uint32_t altura = 0;

    // Do momento em que a área de trabalho apresentou o quadro até agora. É a
    // latência que a captura acrescenta, e a única parte dela que este código
    // controla.
    int64_t latenciaUs = 0;

    // Quantos quadros o DXGI juntou desde a última leitura. Maior que 1
    // significa que a tela mudou mais rápido do que estamos lendo.
    uint32_t quadrosAcumulados = 0;

    // O cursor não vem desenhado no quadro: o DXGI entrega a área de trabalho
    // sem ele e o ponteiro por um caminho separado. Estes campos são o que
    // permite compor um por cima do outro.
    bool cursorVisivel = false;
    int32_t cursorX = 0;
    int32_t cursorY = 0;

    // Verdadeiro quando a forma do cursor mudou neste quadro — só aí vale a
    // pena reenviá-la para a GPU.
    bool formaMudou = false;
};

enum class ResultadoQuadro {
    Ok,
    SemMudanca,       // nada mudou na tela dentro do prazo; não é erro
    PrecisaReiniciar, // troca de resolução, tela cheia exclusiva, UAC
    Erro,
};

class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();

    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    // Lista os monitores disponíveis, sem precisar iniciar a captura.
    static std::vector<MonitorInfo> listarMonitores();

    // Prepara o dispositivo D3D11 do adaptador do monitor e tenta abrir a
    // duplicação.
    //
    // Só falha quando o dispositivo não pôde ser criado. A duplicação em si
    // pode não abrir agora — durante o UAC, na tela de bloqueio, com jogo em
    // tela cheia exclusiva — e isso não é motivo para o aplicativo desistir:
    // dá para entrar na sala, assistir e usar a câmera sem ela. Quando faltar,
    // proximoQuadro() devolve PrecisaReiniciar e ela é aberta depois.
    bool iniciar(uint32_t indiceMonitor);

    // Abre a duplicação num dispositivo D3D que já existe, em vez de criar o
    // seu. É o que permite transmitir dois monitores ao mesmo tempo: o Video
    // Processor compõe as duas texturas numa passada só, e para isso elas
    // precisam ser do mesmo dispositivo.
    //
    // Falha quando o monitor está em outro adaptador — placa híbrida com uma
    // tela em cada. Quem chama segue sem esse monitor.
    bool iniciarCom(ID3D11Device* dispositivo, ID3D11DeviceContext* contexto,
                    uint32_t indiceMonitor);

    void parar();

    // Verdadeiro quando a duplicação está aberta e dá para capturar agora.
    bool capturando() const;

    // Verdadeiro quando o dispositivo é WARP — o rasterizador por software.
    //
    // Isto separa dois problemas que davam o mesmo sintoma na tela: "a
    // duplicação está negada agora" (UAC, tela de bloqueio, jogo em tela cheia
    // exclusiva), que passa sozinho, e "não há GPU utilizável", que não passa.
    // O segundo acontece depois que o driver de vídeo cai e se recupera: o
    // Windows registra o tombo, o D3D11 passa a recusar todos os adaptadores
    // com DXGI_ERROR_UNSUPPORTED, e só volta ao normal reiniciando. Sem
    // distinguir, o aplicativo ficava tentando abrir a duplicação para sempre,
    // sem nada na tela e sem dizer por quê.
    bool semGPU() const;

    // Recria a duplicação depois de ResultadoQuadro::PrecisaReiniciar.
    //
    // Tem intervalo mínimo entre tentativas: quando a duplicação está negada de
    // verdade, o laço pediria reinício a cada quadro e viraria uma tentativa a
    // cada poucos milissegundos, enchendo o log e queimando CPU à toa.
    bool reiniciar();

    // Bloqueia até haver quadro novo ou estourar o prazo. Cada Ok precisa de um
    // liberarQuadro() antes da próxima chamada: é exigência do DXGI.
    ResultadoQuadro proximoQuadro(uint32_t prazoMs, QuadroCapturado& saida);
    void liberarQuadro();

    // A forma atual do cursor, já convertida para BGRA. Vazia enquanto o
    // Windows não tiver entregado nenhuma.
    const FormaCursor& formaDoCursor() const;

    ID3D11Device* dispositivo() const;
    ID3D11DeviceContext* contexto() const;
    const MonitorInfo& monitor() const;

private:
    struct Interno;
    std::unique_ptr<Interno> d_;
};

}  // namespace gl
