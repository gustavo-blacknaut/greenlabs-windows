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
                 Saida formatoSaida = Saida::Nv12);

    // A textura devolvida pertence ao conversor e é reaproveitada a cada
    // quadro — quem consome precisa fazê-lo antes da próxima chamada.
    ID3D11Texture2D* converter(ID3D11Texture2D* entrada);

    uint32_t largura() const;
    uint32_t altura() const;

private:
    struct Interno;
    std::unique_ptr<Interno> d_;
};

}  // namespace gl
