#pragma once

// Encoder H.264 por hardware, via Media Foundation.
//
// Uma API só para NVENC, AMF e QuickSync: o Media Foundation escolhe o encoder
// da placa que estiver na máquina. Só cai para software quando não há nenhum.
//
// H.264 é a escolha porque é o único codec que todo lado da chamada tem por
// hardware: Windows pelo Media Foundation, Android pelo MediaCodec (obrigatório
// pelo CDD desde as primeiras versões) e todos os navegadores. Nenhum outro
// codec fecha esse conjunto.

#include <cstdint>
#include <functional>
#include <memory>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace gl {

struct ConfigEncoder {
    uint32_t largura = 1920;
    uint32_t altura = 1080;
    uint32_t fps = 30;
    uint32_t bitrate = 4'500'000;

    // Distância entre quadros-chave. Em tempo real não vale forçar: quando
    // alguém entra no meio da transmissão, o pedido de quadro-chave vem por
    // RTCP e é atendido na hora. Zero deixa o encoder decidir.
    uint32_t intervaloChaveSegundos = 0;
};

struct PacoteCodificado {
    const uint8_t* dados = nullptr;  // emprestado: vale durante o callback
    size_t tamanho = 0;
    int64_t tempoUs = 0;
    bool chave = false;  // IDR: serve de ponto de entrada para quem chega agora
};

class VideoEncoder {
public:
    using Consumidor = std::function<void(const PacoteCodificado&)>;

    VideoEncoder();
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    bool iniciar(ID3D11Device* dispositivo, const ConfigEncoder& config, Consumidor consumidor);
    void parar();

    // A textura precisa ser NV12 e do mesmo dispositivo D3D11. Nada é copiado
    // para a memória principal.
    bool codificar(ID3D11Texture2D* nv12, int64_t tempoUs);

    // Força um quadro-chave no próximo codificar(). É o que responde a um
    // pedido de keyframe de quem acabou de entrar na sala.
    void pedirQuadroChave();

    bool porHardware() const;
    const char* nomeDoEncoder() const;
    uint64_t quadrosCodificados() const;
    uint64_t bytesGerados() const;

    // Quadros largados porque o encoder ainda estava ocupado. Alguns são
    // normais; muitos significam resolução ou bitrate acima do que a placa dá.
    uint64_t quadrosDescartados() const;

private:
    struct Interno;
    std::unique_ptr<Interno> d_;
};

}  // namespace gl
