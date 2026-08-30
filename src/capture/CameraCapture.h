#pragma once

// Captura de câmera com Media Foundation.
//
// Só imagem. A enumeração pede exclusivamente fontes de vídeo, então o
// microfone da webcam nunca é aberto — nem por engano, nem como efeito
// colateral de abrir a câmera. O som que sai daqui continua sendo só o do
// sistema, e é o AudioCapture quem cuida dele.
//
// O quadro é entregue como ID3D11Texture2D NV12, pronta para entrar no Video
// Processor junto com a tela. A leitura acontece numa thread própria: a câmera
// entrega no ritmo dela, que não é o ritmo da tela, e esperar por ela dentro do
// laço da interface faria a captura de tela andar no passo da webcam.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace gl {

struct CameraInfo {
    // O caminho simbólico do dispositivo. É o que fica guardado no config: o
    // nome muda de idioma e de driver, o caminho não.
    std::string id;
    std::string nome;
};

class CameraCapture {
public:
    CameraCapture();
    ~CameraCapture();

    CameraCapture(const CameraCapture&) = delete;
    CameraCapture& operator=(const CameraCapture&) = delete;

    // Lista as câmeras do computador. Não abre nenhuma: dá para montar a lista
    // da interface sem acender a luzinha de ninguém.
    static std::vector<CameraInfo> listar();

    // Abre a câmera e começa a ler. id vazio pega a primeira que houver.
    //
    // Falha sem drama quando a câmera está em uso por outro programa — que é o
    // caso comum de quem tem o Discord aberto. Quem chama segue sem câmera.
    bool iniciar(const std::string& id, ID3D11Device* dispositivo,
                 ID3D11DeviceContext* contexto);
    void parar();
    bool ativa() const;

    // O quadro mais recente, ou nullptr enquanto não houver nenhum.
    //
    // A textura pertence à captura e vale até a próxima troca de quadro. Ela é
    // alternada entre duas, então quem desenha a partir dela tem um quadro
    // inteiro de folga — o suficiente para uma passada do Video Processor.
    ID3D11Texture2D* quadro();

    uint32_t largura() const;
    uint32_t altura() const;

    // Nome da câmera aberta, para a interface mostrar sem consultar a lista.
    const std::string& nome() const;

private:
    struct Interno;
    std::unique_ptr<Interno> d_;
};

}  // namespace gl
