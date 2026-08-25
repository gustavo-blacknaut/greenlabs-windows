#include "network/WebSocketClient.h"

#include <winsock2.h>
// ws2tcpip.h precisa vir depois do winsock2.h, senão o windows.h antigo entra
// no meio e o compilador reclama de redefinição.
#include <ws2tcpip.h>

#include <bcrypt.h>
#include <wincrypt.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "util/Log.h"

namespace gl {
namespace {

constexpr const char* kGuidWebSocket = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

constexpr uint8_t opTexto = 0x1;
constexpr uint8_t opFechar = 0x8;
constexpr uint8_t opPing = 0x9;
constexpr uint8_t opPong = 0xA;

// Sinalização é SDP e ICE: alguns KB. O teto existe para um servidor hostil não
// conseguir pedir um alloc gigante.
constexpr size_t kTamanhoMaximo = 1 << 20;

// O Winsock precisa ser inicializado uma vez por processo, e desligado no fim.
struct Winsock {
    Winsock() {
        WSADATA dados{};
        ok = ::WSAStartup(MAKEWORD(2, 2), &dados) == 0;
    }
    ~Winsock() {
        if (ok) ::WSACleanup();
    }
    bool ok = false;
};

void garantirWinsock() { static Winsock unico; }

std::string base64(const uint8_t* dados, size_t tamanho) {
    DWORD necessario = 0;
    ::CryptBinaryToStringA(dados, static_cast<DWORD>(tamanho),
                           CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &necessario);
    std::string saida(necessario, '\0');
    ::CryptBinaryToStringA(dados, static_cast<DWORD>(tamanho),
                           CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, saida.data(), &necessario);
    saida.resize(necessario);
    while (!saida.empty() && (saida.back() == '\0' || saida.back() == '\n' || saida.back() == '\r')) {
        saida.pop_back();
    }
    return saida;
}

void bytesAleatorios(uint8_t* destino, size_t tamanho) {
    if (::BCryptGenRandom(nullptr, destino, static_cast<ULONG>(tamanho),
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
        return;
    }
    // Improvável, mas uma máscara previsível é melhor que máscara nenhuma.
    for (size_t i = 0; i < tamanho; ++i) {
        destino[i] = static_cast<uint8_t>(::GetTickCount() * (i + 7));
    }
}

struct Endereco {
    std::string host;
    std::string porta = "80";
    std::string caminho = "/";
    bool seguro = false;
};

Endereco separarEndereco(const std::string& bruto) {
    Endereco e;
    std::string resto = bruto;

    if (resto.rfind("wss://", 0) == 0) {
        e.seguro = true;
        e.porta = "443";
        resto = resto.substr(6);
    } else if (resto.rfind("ws://", 0) == 0) {
        resto = resto.substr(5);
    }

    const size_t barra = resto.find('/');
    if (barra != std::string::npos) {
        e.caminho = resto.substr(barra);
        resto = resto.substr(0, barra);
    }

    const size_t doisPontos = resto.rfind(':');
    // rfind e não find: endereço IPv6 tem dois-pontos no meio.
    if (doisPontos != std::string::npos && resto.find(']') < doisPontos) {
        e.host = resto.substr(0, doisPontos);
        e.porta = resto.substr(doisPontos + 1);
    } else if (doisPontos != std::string::npos && resto.find('[') == std::string::npos) {
        e.host = resto.substr(0, doisPontos);
        e.porta = resto.substr(doisPontos + 1);
    } else {
        e.host = resto;
    }
    return e;
}

}  // namespace

struct WebSocketClient::Interno {
    SOCKET soquete = INVALID_SOCKET;
    std::atomic<bool> ativo{false};
    std::thread leitor;

    std::mutex travaEscrita;
    AoReceber aoReceber;
    AoFechar aoFechar;

    std::atomic<uint64_t> recebidas{0};
    std::atomic<uint64_t> enviadas{0};

    std::vector<uint8_t> buffer;  // bytes lidos mas ainda não consumidos
    std::vector<uint8_t> montagem;
    bool montando = false;

    bool lerExato(uint8_t* destino, size_t quantos);
    bool escreverTudo(const uint8_t* dados, size_t tamanho);
    bool enviarQuadro(uint8_t opcode, const uint8_t* dados, size_t tamanho);
    void laco();
    void encerrar(const std::string& motivo);
};

WebSocketClient::WebSocketClient() : d_(std::make_unique<Interno>()) {}
WebSocketClient::~WebSocketClient() { fechar(); }

void WebSocketClient::aoReceber(AoReceber cb) { d_->aoReceber = std::move(cb); }
void WebSocketClient::aoFechar(AoFechar cb) { d_->aoFechar = std::move(cb); }
bool WebSocketClient::conectado() const { return d_->ativo.load(); }
uint64_t WebSocketClient::mensagensRecebidas() const { return d_->recebidas.load(); }
uint64_t WebSocketClient::mensagensEnviadas() const { return d_->enviadas.load(); }

bool WebSocketClient::conectar(const std::string& endereco, uint32_t prazoMs) {
    garantirWinsock();
    fechar();

    const Endereco alvo = separarEndereco(endereco);
    if (alvo.seguro) {
        erro("wss:// ainda nao e suportado pelo cliente nativo; use ws://");
        return false;
    }
    if (alvo.host.empty()) {
        erro("endereco de servidor vazio");
        return false;
    }

    addrinfo dicas{};
    dicas.ai_family = AF_UNSPEC;
    dicas.ai_socktype = SOCK_STREAM;
    dicas.ai_protocol = IPPROTO_TCP;

    addrinfo* encontrados = nullptr;
    if (::getaddrinfo(alvo.host.c_str(), alvo.porta.c_str(), &dicas, &encontrados) != 0) {
        erro("nao foi possivel resolver {}:{}", alvo.host, alvo.porta);
        return false;
    }

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* a = encontrados; a; a = a->ai_next) {
        s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) break;
        ::closesocket(s);
        s = INVALID_SOCKET;
    }
    ::freeaddrinfo(encontrados);

    if (s == INVALID_SOCKET) {
        erro("nao foi possivel conectar em {}:{}", alvo.host, alvo.porta);
        return false;
    }

    // Sinalização é feita de mensagens pequenas onde latência é tudo: agrupar
    // pacotes por Nagle só adiciona espera.
    BOOL semAtraso = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&semAtraso),
                 sizeof(semAtraso));
    DWORD prazo = prazoMs;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&prazo), sizeof(prazo));

    d_->soquete = s;

    uint8_t chaveBruta[16];
    bytesAleatorios(chaveBruta, sizeof(chaveBruta));
    const std::string chave = base64(chaveBruta, sizeof(chaveBruta));

    const std::string pedido =
        "GET " + alvo.caminho + " HTTP/1.1\r\n" +
        "Host: " + alvo.host + ":" + alvo.porta + "\r\n" +
        "Upgrade: websocket\r\n" +
        "Connection: Upgrade\r\n" +
        "Sec-WebSocket-Key: " + chave + "\r\n" +
        "Sec-WebSocket-Version: 13\r\n" +
        "User-Agent: GreenLabs-Nativo\r\n\r\n";

    if (!d_->escreverTudo(reinterpret_cast<const uint8_t*>(pedido.data()), pedido.size())) {
        erro("falha ao enviar o handshake");
        fechar();
        return false;
    }

    // Lê a resposta até a linha em branco. O que vier depois já é quadro, e
    // precisa ficar no buffer — descartar aqui perderia a primeira mensagem.
    std::string cabecalhos;
    uint8_t pedaco[2048];
    for (;;) {
        const int lidos = ::recv(d_->soquete, reinterpret_cast<char*>(pedaco), sizeof(pedaco), 0);
        if (lidos <= 0) {
            erro("servidor fechou durante o handshake");
            fechar();
            return false;
        }
        cabecalhos.append(reinterpret_cast<char*>(pedaco), static_cast<size_t>(lidos));
        const size_t fim = cabecalhos.find("\r\n\r\n");
        if (fim != std::string::npos) {
            const size_t inicioCorpo = fim + 4;
            if (inicioCorpo < cabecalhos.size()) {
                d_->buffer.assign(cabecalhos.begin() + static_cast<ptrdiff_t>(inicioCorpo),
                                  cabecalhos.end());
            }
            cabecalhos.resize(fim);
            break;
        }
        if (cabecalhos.size() > 16384) {
            erro("resposta de handshake grande demais");
            fechar();
            return false;
        }
    }

    if (cabecalhos.find("101") == std::string::npos) {
        const size_t fimLinha = cabecalhos.find("\r\n");
        erro("handshake recusado: {}", cabecalhos.substr(0, fimLinha));
        fechar();
        return false;
    }

    // Sem prazo de leitura no laço: a conexão fica aberta por horas e o ping do
    // servidor é o que prova que ela está viva.
    DWORD semPrazo = 0;
    ::setsockopt(d_->soquete, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&semPrazo), sizeof(semPrazo));

    d_->ativo.store(true);
    d_->leitor = std::thread([this] { d_->laco(); });

    info("sinalizacao conectada em ws://{}:{}{}", alvo.host, alvo.porta, alvo.caminho);
    return true;
}

bool WebSocketClient::Interno::escreverTudo(const uint8_t* dados, size_t tamanho) {
    size_t enviado = 0;
    while (enviado < tamanho) {
        const int n = ::send(soquete, reinterpret_cast<const char*>(dados + enviado),
                             static_cast<int>(tamanho - enviado), 0);
        if (n <= 0) return false;
        enviado += static_cast<size_t>(n);
    }
    return true;
}

bool WebSocketClient::Interno::lerExato(uint8_t* destino, size_t quantos) {
    size_t obtidos = 0;

    // Primeiro o que já veio grudado num recv anterior.
    if (!buffer.empty()) {
        const size_t doBuffer = quantos < buffer.size() ? quantos : buffer.size();
        std::copy(buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(doBuffer), destino);
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(doBuffer));
        obtidos = doBuffer;
    }

    while (obtidos < quantos) {
        const int n = ::recv(soquete, reinterpret_cast<char*>(destino + obtidos),
                             static_cast<int>(quantos - obtidos), 0);
        if (n <= 0) return false;
        obtidos += static_cast<size_t>(n);
    }
    return true;
}

bool WebSocketClient::Interno::enviarQuadro(uint8_t opcode, const uint8_t* dados, size_t tamanho) {
    if (!ativo.load()) return false;

    uint8_t cabecalho[14];
    size_t n = 0;
    cabecalho[n++] = static_cast<uint8_t>(0x80 | opcode);  // FIN: não fragmentamos

    // O bit de máscara é obrigatório em tudo que sai do cliente.
    if (tamanho <= 125) {
        cabecalho[n++] = static_cast<uint8_t>(0x80 | tamanho);
    } else if (tamanho <= 0xFFFF) {
        cabecalho[n++] = 0x80 | 126;
        cabecalho[n++] = static_cast<uint8_t>((tamanho >> 8) & 0xFF);
        cabecalho[n++] = static_cast<uint8_t>(tamanho & 0xFF);
    } else {
        cabecalho[n++] = 0x80 | 127;
        for (int i = 7; i >= 0; --i) {
            cabecalho[n++] = static_cast<uint8_t>((static_cast<uint64_t>(tamanho) >> (i * 8)) & 0xFF);
        }
    }

    uint8_t mascara[4];
    bytesAleatorios(mascara, sizeof(mascara));
    for (uint8_t b : mascara) cabecalho[n++] = b;

    std::vector<uint8_t> corpo(tamanho);
    for (size_t i = 0; i < tamanho; ++i) corpo[i] = dados[i] ^ mascara[i & 3];

    std::lock_guard trava(travaEscrita);
    return escreverTudo(cabecalho, n) && (tamanho == 0 || escreverTudo(corpo.data(), corpo.size()));
}

bool WebSocketClient::enviar(const std::string& mensagem) {
    if (!d_->enviarQuadro(opTexto, reinterpret_cast<const uint8_t*>(mensagem.data()),
                          mensagem.size())) {
        return false;
    }
    d_->enviadas.fetch_add(1);
    return true;
}

void WebSocketClient::Interno::laco() {
    while (ativo.load()) {
        uint8_t cabecalho[2];
        if (!lerExato(cabecalho, 2)) break;

        const bool fim = (cabecalho[0] & 0x80) != 0;
        const uint8_t opcode = cabecalho[0] & 0x0F;
        const bool mascarado = (cabecalho[1] & 0x80) != 0;
        uint64_t tamanho = cabecalho[1] & 0x7F;

        if (tamanho == 126) {
            uint8_t ext[2];
            if (!lerExato(ext, 2)) break;
            tamanho = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (tamanho == 127) {
            uint8_t ext[8];
            if (!lerExato(ext, 8)) break;
            tamanho = 0;
            for (uint8_t b : ext) tamanho = (tamanho << 8) | b;
        }

        if (mascarado) {
            // Servidor não pode mascarar. Se mascarou, não é um servidor
            // WebSocket falando o protocolo direito.
            encerrar("servidor mascarou um quadro");
            return;
        }
        if (tamanho > kTamanhoMaximo) {
            encerrar("quadro grande demais");
            return;
        }

        std::vector<uint8_t> dados(static_cast<size_t>(tamanho));
        if (tamanho > 0 && !lerExato(dados.data(), dados.size())) break;

        if (opcode == opPing) {
            // Responder ping é obrigação do cliente pela RFC, e é o que mantém
            // o servidor sabendo que estamos vivos.
            enviarQuadro(opPong, dados.data(), dados.size());
            continue;
        }
        if (opcode == opPong) continue;
        if (opcode == opFechar) {
            enviarQuadro(opFechar, nullptr, 0);
            encerrar("servidor encerrou a conexao");
            return;
        }

        if (opcode == opTexto || opcode == 0x2) {
            if (montando) {
                encerrar("mensagem nova antes do fim da anterior");
                return;
            }
            montagem = std::move(dados);
            montando = true;
        } else if (opcode == 0x0) {
            if (!montando) {
                encerrar("continuacao sem inicio");
                return;
            }
            if (montagem.size() + dados.size() > kTamanhoMaximo) {
                encerrar("mensagem fragmentada grande demais");
                return;
            }
            montagem.insert(montagem.end(), dados.begin(), dados.end());
        } else {
            encerrar("opcode desconhecido");
            return;
        }

        if (fim) {
            recebidas.fetch_add(1);
            if (aoReceber) {
                aoReceber(std::string(reinterpret_cast<char*>(montagem.data()), montagem.size()));
            }
            montagem.clear();
            montando = false;
        }
    }

    encerrar("conexao caiu");
}

void WebSocketClient::Interno::encerrar(const std::string& motivo) {
    if (!ativo.exchange(false)) return;
    if (soquete != INVALID_SOCKET) {
        ::shutdown(soquete, SD_BOTH);
    }
    if (aoFechar) aoFechar(motivo);
}

void WebSocketClient::fechar() {
    if (d_->ativo.load()) {
        d_->enviarQuadro(opFechar, nullptr, 0);
    }
    d_->ativo.store(false);

    if (d_->soquete != INVALID_SOCKET) {
        ::shutdown(d_->soquete, SD_BOTH);
        ::closesocket(d_->soquete);
        d_->soquete = INVALID_SOCKET;
    }
    if (d_->leitor.joinable() && d_->leitor.get_id() != std::this_thread::get_id()) {
        d_->leitor.join();
    }
    d_->buffer.clear();
    d_->montagem.clear();
    d_->montando = false;
}

}  // namespace gl
