#pragma once

// Conversão BGRA → NV12 na GPU, usando o Video Processor do D3D11.
//
// A duplicação de área de trabalho entrega BGRA; o encoder H.264 quer NV12.
// Fazer essa conversão na CPU custaria uma leitura da GPU para a memória
// principal por quadro — a cópia mais cara do pipeline inteiro, e justamente a
// que o desenho do cliente existe para evitar.
//
// O Video Processor é a unidade de função fixa da placa: a conversão acontece
// no mesmo lugar onde o quadro já está, e a textura resultante vai direto para
// o encoder.

#include <cstdint>
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace gl {

class ColorConverter {
public:
    ColorConverter();
    ~ColorConverter();

    ColorConverter(const ColorConverter&) = delete;
    ColorConverter& operator=(const ColorConverter&) = delete;

    // largura e altura de saída podem diferir da entrada: o Video Processor
    // redimensiona de graça, no mesmo passo da conversão.
    // formatoSaida escolhe entre NV12 (o que o encoder consome) e BGRA (o que
    // o Direct2D sabe desenhar). O mesmo Video Processor faz os dois sentidos,
    // e no caminho de exibição a conversão é de NV12 para BGRA.
    enum class Saida { Nv12, Bgra };

    bool iniciar(ID3D11Device* dispositivo, ID3D11DeviceContext* contexto,
                 uint32_t larguraEntrada, uint32_t alturaEntrada,
                 uint32_t larguraSaida, uint32_t alturaSaida,
                 Saida formatoSaida = Saida::Nv12, uint32_t graus = 0);

    // Liga a segunda entrada: a câmera, desenhada num canto do quadro.
    //
    // É o Video Processor quem compõe, na mesma passada da conversão de cor.
    // Fazer a composição depois, num shader ou na CPU, custaria uma passada
    // inteira sobre 1080p por quadro; aqui custa zero, porque a unidade de
    // função fixa da placa já está lendo os dois de qualquer jeito.
    //
    // Devolve falso quando a placa não aceita duas entradas ou não aceita NV12
    // na entrada — e aí quem chama transmite só a tela, sem câmera.
    bool prepararSobreposicao(uint32_t larguraCamera, uint32_t alturaCamera);
    void desligarSobreposicao();

    // A textura devolvida pertence ao conversor e é reaproveitada a cada
    // quadro — quem consome precisa fazê-lo antes da próxima chamada.
    //
    // sobreposicao é opcional: quando vem, entra no canto combinado em
    // prepararSobreposicao. Passar nullptr desenha só a entrada principal, o
    // que é o caso de todo quadro em que a câmera ainda não entregou nada.
    ID3D11Texture2D* converter(ID3D11Texture2D* entrada,
                               ID3D11Texture2D* sobreposicao = nullptr);

    uint32_t largura() const;
    uint32_t altura() const;

private:
    struct Interno;
    std::unique_ptr<Interno> d_;
};

}  // namespace gl
