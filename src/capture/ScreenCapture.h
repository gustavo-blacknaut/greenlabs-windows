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

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace gl {

struct MonitorInfo {
    uint32_t indice = 0;
    std::string nome;
    uint32_t largura = 0;
    uint32_t altura = 0;
    bool primario = false;
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

    // O cursor NAO vem desenhado no quadro: o DXGI entrega a área de trabalho
    // sem ele. Estas posições existem para compor o cursor depois; enquanto
    // isso não for feito, a transmissão sai sem ponteiro do mouse.
    bool cursorVisivel = false;
    int32_t cursorX = 0;
    int32_t cursorY = 0;
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

    bool iniciar(uint32_t indiceMonitor);
    void parar();

    // Recria a duplicação depois de ResultadoQuadro::PrecisaReiniciar.
    bool reiniciar();

    // Bloqueia até haver quadro novo ou estourar o prazo. Cada Ok precisa de um
    // liberarQuadro() antes da próxima chamada: é exigência do DXGI.
    ResultadoQuadro proximoQuadro(uint32_t prazoMs, QuadroCapturado& saida);
    void liberarQuadro();

    ID3D11Device* dispositivo() const;
    ID3D11DeviceContext* contexto() const;
    const MonitorInfo& monitor() const;

private:
    struct Interno;
    std::unique_ptr<Interno> d_;
};

}  // namespace gl
