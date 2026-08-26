#pragma once

// Composição do cursor sobre o quadro capturado.
//
// A duplicação de área de trabalho entrega a tela SEM o ponteiro do mouse, e a
// forma dele por um caminho separado. Quem transmite tela precisa que o cursor
// apareça — sem ele quem assiste não sabe para onde a pessoa está apontando.
//
// O desenho acontece na GPU, sobre uma cópia do quadro. É uma cópia a mais por
// quadro, dentro da própria placa, e é o preço de ter o ponteiro.

#include <cstdint>
#include <memory>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace gl {

// A forma do cursor, já convertida para BGRA. O Windows entrega em três
// formatos diferentes (colorido, monocromático e mascarado); a conversão
// deixa todos iguais aqui.
struct FormaCursor {
    std::vector<uint8_t> pixels;  // BGRA
    uint32_t largura = 0;
    uint32_t altura = 0;
    int32_t ancoraX = 0;  // ponto quente: onde na imagem fica a "ponta"
    int32_t ancoraY = 0;
    bool valida() const { return largura > 0 && altura > 0 && !pixels.empty(); }
};

class Cursor {
public:
    Cursor();
    ~Cursor();

    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;

    bool iniciar(ID3D11Device* dispositivo, ID3D11DeviceContext* contexto, uint32_t largura,
                 uint32_t altura);

    // Troca a forma desenhada. Só precisa ser chamado quando o cursor muda.
    void definirForma(const FormaCursor& forma);

    // Devolve uma cópia do quadro com o cursor desenhado por cima. Quando não há
    // forma ou o cursor está escondido, devolve o próprio quadro sem copiar.
    ID3D11Texture2D* compor(ID3D11Texture2D* quadro, bool visivel, int32_t x, int32_t y);

private:
    struct Interno;
    std::unique_ptr<Interno> d_;
};

}  // namespace gl
