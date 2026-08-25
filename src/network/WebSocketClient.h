#pragma once

// Cliente WebSocket (RFC 6455), o suficiente para a sinalização.
//
// Feito à mão pelo mesmo motivo do servidor em Go: nenhuma dependência externa,
// então "git clone && build" produz um executável sem baixar nada.
//
// Diferença importante do lado servidor: todo quadro que sai do cliente
// **precisa** ser mascarado, e a máscara tem que ser aleatória de verdade a
// cada quadro. Proxies rejeitam a conexão quando não é.
//
// Só ws:// por enquanto. O wss:// entra junto com o DTLS da etapa de mídia, que
// já traz a camada de TLS.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace gl {

class WebSocketClient {
public:
    using AoReceber = std::function<void(const std::string& mensagem)>;
    using AoFechar = std::function<void(const std::string& motivo)>;

    WebSocketClient();
    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    // Aceita "host:porta", "ws://host:porta" e "ws://host:porta/caminho".
    // Bloqueia até o handshake terminar ou estourar o prazo.
    bool conectar(const std::string& endereco, uint32_t prazoMs = 8000);

    // Enfileira uma mensagem de texto. Pode ser chamado de qualquer thread.
    bool enviar(const std::string& mensagem);

    void fechar();
    bool conectado() const;

    // Os dois são chamados na thread de leitura. Não bloquear dentro deles.
    void aoReceber(AoReceber cb);
    void aoFechar(AoFechar cb);

    uint64_t mensagensRecebidas() const;
    uint64_t mensagensEnviadas() const;

private:
    struct Interno;
    std::unique_ptr<Interno> d_;
};

}  // namespace gl
