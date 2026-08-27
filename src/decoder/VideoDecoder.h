#pragma once

// Decodificação de H.264 pela GPU, com Media Foundation.
//
// É o caminho inverso do VideoEncoder: entra Annex-B (00 00 00 01 antes de cada
// unidade NAL, que é como a rede entrega) e sai uma textura NV12 na memória de
// vídeo, pronta para o renderizador.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace gl {

class VideoDecoder {
public:
    // Chamado a cada quadro pronto, da thread que entregou os dados. A textura
    // pertence ao decodificador e vale até a próxima chamada - quem quiser
    // guardar precisa copiar.
    using Consumidor = std::function<void(ID3D11Texture2D* nv12, uint32_t largura,
                                          uint32_t altura)>;

    VideoDecoder();
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // O dispositivo é o mesmo da captura e do desenho: assim a textura decodada
    // vai direto para a tela, sem passar pela memória principal.
    bool iniciar(ID3D11Device* dispositivo, Consumidor consumidor);
    void parar();

    // Entrega um quadro em Annex-B. A resolução não precisa ser informada: ela
    // vem no SPS, e o decodificador se reconfigura sozinho quando muda.
    void decodificar(const uint8_t* dados, size_t tamanho, int64_t tempoUs);

    bool ativo() const;
    const std::string& nome() const;
    uint64_t quadrosDecodificados() const;

private:
    struct Interno;
    std::unique_ptr<Interno> d_;
};

}  // namespace gl
