#pragma once

// Cliente de sinalização do GreenLabs.
//
// Mesmo protocolo do servidor em Go e dos clientes em Electron, Android e
// navegador: JSON sobre WebSocket, sem autenticação. Este arquivo é a tradução
// direta do que o cliente web faz, e não inventa nada — trocar qualquer campo
// aqui quebra a conversa com os outros três.
//
// Entrada:
//   {"type":"join","roomId":"call1","name":"Fulano"}
//   {"type":"ping","timestamp":<ms>,"rtt":<ms>}
//   {"type":"offer"|"answer"|"ice"|"stream-meta"|"stream-ended","to":"<peerId>", ...}
//
// Saída:
//   {"type":"joined","peerId":"...","peers":[...],"count":N}
//   {"type":"peer-joined"|"peer-left"|"pong"|"room-pings", ...}
//   qualquer repasse chega com "from" carimbado pelo servidor.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "util/Json.h"

namespace gl {

struct Participante {
    std::string id;
    std::string nome;
    int64_t pingMs = 0;
};

class Signaling {
public:
    struct Ouvintes {
        // Entramos na sala. `eu` é o id que o servidor deu.
        std::function<void(const std::string& eu, const std::vector<Participante>&)> aoEntrar;
        std::function<void(const Participante&)> aoChegarAlguem;
        std::function<void(const std::string& peerId)> aoSairAlguem;
        // Repasse ponto a ponto: offer, answer, ice, stream-meta, stream-ended.
        std::function<void(const std::string& de, const Json& mensagem)> aoRepasse;
        std::function<void(const std::string& motivo)> aoCair;
    };

    Signaling();
    ~Signaling();

    Signaling(const Signaling&) = delete;
    Signaling& operator=(const Signaling&) = delete;

    void definirOuvintes(Ouvintes ouvintes);

    // Conecta e já entra na sala. Bloqueia só até o WebSocket abrir; o "joined"
    // chega depois, pelo ouvinte.
    bool entrar(const std::string& servidor, const std::string& sala, const std::string& nome);
    void sair();

    bool conectado() const;

    // Envia uma mensagem para um participante. O servidor carimba o "from".
    bool enviarPara(const std::string& peerId, Json mensagem);

    std::string meuId() const;
    std::vector<Participante> participantes() const;
    int64_t pingMs() const;

private:
    struct Interno;
    std::unique_ptr<Interno> d_;
};

}  // namespace gl
