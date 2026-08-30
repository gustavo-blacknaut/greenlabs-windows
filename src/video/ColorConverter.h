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
#include <vector>

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

    // Onde cada imagem cai dentro do quadro final, em pixels da saída.
    struct Pedaco {
        int32_t esquerda = 0;
        int32_t topo = 0;
        int32_t direita = 0;
        int32_t baixo = 0;

        // Giro do monitor de onde esta imagem veio. Cada tela tem o seu: um
        // monitor em pe ao lado de um deitado e o caso comum de quem tem dois.
        uint32_t graus = 0;
    };

    // Monta a composição: uma entrada por pedaço, na mesma ordem.
    //
    // É o Video Processor quem compõe, na mesma passada da conversão de cor.
    // Fazer isso depois, num shader ou na CPU, custaria uma passada inteira
    // sobre o quadro; aqui custa zero, porque a unidade de função fixa da placa
    // já está lendo tudo de qualquer jeito.
    //
    // É assim que duas telas cabem lado a lado e a câmera cabe no canto, sem
    // nada disso virar uma segunda faixa de vídeo — que o servidor não aceita.
    //
    // Devolve falso quando a placa não aceita tantas entradas ou não aceita os
    // formatos; aí quem chama compõe menos coisa.
    bool prepararComposicao(const std::vector<Pedaco>& pedacos);
    void desligarComposicao();

    // A textura devolvida pertence ao conversor e é reaproveitada a cada
    // quadro — quem consome precisa fazê-lo antes da próxima chamada.
    //
    // Sem composição montada, usa só a primeira entrada, no quadro inteiro.
    // Entrada nula é pulada: é o caso de todo quadro em que uma das telas (ou
    // a câmera) ainda não entregou nada, e o resto continua saindo.
    ID3D11Texture2D* compor(const std::vector<ID3D11Texture2D*>& entradas);

    // Atalho para o caminho de uma imagem só, que é o da exibição.
    ID3D11Texture2D* converter(ID3D11Texture2D* entrada) { return compor({entrada}); }

    uint32_t largura() const;
    uint32_t altura() const;

private:
    struct Interno;
    std::unique_ptr<Interno> d_;
};

}  // namespace gl
