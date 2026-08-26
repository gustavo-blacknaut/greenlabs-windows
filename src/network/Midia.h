#pragma once

// Transporte de mídia: leva o H.264 do encoder até o outro lado da chamada.
//
// Por baixo é WebRTC de verdade — ICE para atravessar o roteador, DTLS para
// trocar as chaves, SRTP para cifrar cada pacote. É o que permite conversar com
// o navegador e com o app Android, que só falam isso.
//
// A escolha de H.264 vem daí: é o único codec que os três lados têm por
// hardware. Trocar por VP8 ou AV1 significaria software em pelo menos um deles.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace gl {

struct ConfigMidia {
    uint32_t largura = 1920;
    uint32_t altura = 1080;
    uint32_t fps = 30;
    uint32_t bitrate = 4'500'000;
};

// Uma conexão com um participante. Numa sala de N pessoas existem N-1 destas.
class ConexaoPar {
public:
    // Chamados de dentro da biblioteca, em thread própria. Não bloquear.
    using AoDescrever = std::function<void(const std::string& tipo, const std::string& sdp)>;
    using AoCandidato = std::function<void(const std::string& candidato, const std::string& mid)>;
    using AoEstado = std::function<void(const std::string& estado)>;

    // O outro lado pediu um quadro-chave (PLI). Acontece quando ele entra no
    // meio da transmissão ou perde pacote demais para se recuperar sozinho.
    using AoPedirChave = std::function<void()>;

    ConexaoPar(std::string idDoPar, const ConfigMidia& config);
    ~ConexaoPar();

    ConexaoPar(const ConexaoPar&) = delete;
    ConexaoPar& operator=(const ConexaoPar&) = delete;

    void aoDescrever(AoDescrever cb);
    void aoCandidato(AoCandidato cb);
    void aoEstado(AoEstado cb);
    void aoPedirChave(AoPedirChave cb);

    // Cria a faixa de vídeo. Precisa vir antes de oferecer OU de responder:
    // uma resposta SDP não pode inventar m-line que a oferta não trouxe, então
    // a faixa tem que existir antes de a descrição remota chegar.
    bool prepararFaixa();

    // Dispara a oferta. Quem tem a tela para mostrar chama isto.
    bool oferecer();

    // Estado da conexão, em uma palavra, para a interface mostrar.
    std::string estado() const;

    // Que caminhos o ICE conseguiu: "local", "publico", "retransmitido".
    // Quando só há "local", quem está fora da rede não consegue receber - e é
    // a informação que falta quando a conexão fica presa procurando caminho.
    std::string caminhos() const;

    // Há uma oferta nossa esperando resposta. Serve para detectar colisão:
    // dois lados oferecendo ao mesmo tempo não conseguem se entender.
    bool ofertaPendente() const;

    void receberDescricao(const std::string& tipo, const std::string& sdp);
    void receberCandidato(const std::string& candidato, const std::string& mid);

    // Manda um quadro já codificado. O empacotamento em RTP (RFC 6184,
    // fragmentação FU-A quando não cabe num pacote) acontece aqui dentro.
    //
    // Quadros antes do primeiro quadro-chave são descartados: o decodificador
    // do outro lado não tem como começar sem um, e mandar fatia P sozinha só
    // gasta banda enquanto ele espera.
    void enviarVideo(const uint8_t* anexoB, size_t tamanho, int64_t tempoUs, bool chave);

    bool pronto() const;
    const std::string& idDoPar() const;
    uint64_t pacotesEnviados() const;
    uint64_t bytesEnviados() const;

private:
    struct Interno;
    std::unique_ptr<Interno> d_;
};

}  // namespace gl
