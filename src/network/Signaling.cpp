#include "network/Signaling.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#include "network/WebSocketClient.h"
#include "util/Log.h"

namespace gl {
namespace {

int64_t agoraMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t monotonicoMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

struct Signaling::Interno {
    WebSocketClient socket;
    Ouvintes ouvintes;

    std::string sala;
    std::string nome;
    std::string eu;

    mutable std::mutex trava;
    std::map<std::string, Participante> pessoas;

    std::atomic<int64_t> ping{0};
    std::atomic<int64_t> enviadoEm{0};
    std::atomic<bool> pingando{false};
    std::thread threadPing;

    void tratar(const std::string& bruto);
    void lacoPing();
};

Signaling::Signaling() : d_(std::make_unique<Interno>()) {}
Signaling::~Signaling() { sair(); }

void Signaling::definirOuvintes(Ouvintes ouvintes) { d_->ouvintes = std::move(ouvintes); }
bool Signaling::conectado() const { return d_->socket.conectado(); }
int64_t Signaling::pingMs() const { return d_->ping.load(); }

std::string Signaling::meuId() const {
    std::lock_guard trava(d_->trava);
    return d_->eu;
}

std::vector<Participante> Signaling::participantes() const {
    std::lock_guard trava(d_->trava);
    std::vector<Participante> lista;
    lista.reserve(d_->pessoas.size());
    for (const auto& [id, p] : d_->pessoas) lista.push_back(p);
    return lista;
}

bool Signaling::entrar(const std::string& servidor, const std::string& sala,
                       const std::string& nome) {
    d_->sala = sala.empty() ? "call1" : sala;
    d_->nome = nome.empty() ? "Usuario" : nome;

    d_->socket.aoReceber([this](const std::string& bruto) { d_->tratar(bruto); });
    d_->socket.aoFechar([this](const std::string& motivo) {
        d_->pingando.store(false);
        if (d_->ouvintes.aoCair) d_->ouvintes.aoCair(motivo);
    });

    if (!d_->socket.conectar(servidor)) return false;

    Json entrada = Json::objeto();
    entrada["type"] = Json{"join"};
    entrada["roomId"] = Json{d_->sala};
    entrada["name"] = Json{d_->nome};
    if (!d_->socket.enviar(entrada.paraTexto())) return false;

    d_->pingando.store(true);
    d_->threadPing = std::thread([this] { d_->lacoPing(); });
    return true;
}

void Signaling::sair() {
    d_->pingando.store(false);
    if (d_->threadPing.joinable()) d_->threadPing.join();
    d_->socket.fechar();

    std::lock_guard trava(d_->trava);
    d_->pessoas.clear();
    d_->eu.clear();
}

bool Signaling::enviarPara(const std::string& peerId, Json mensagem) {
    mensagem["to"] = Json{peerId};
    return d_->socket.enviar(mensagem.paraTexto());
}

void Signaling::Interno::lacoPing() {
    // Um por segundo, igual aos outros clientes. O servidor devolve os pings da
    // sala inteira uma vez por segundo, e é assim que cada um vê a latência dos
    // outros.
    while (pingando.load()) {
        Json p = Json::objeto();
        p["type"] = Json{"ping"};
        p["timestamp"] = Json{agoraMs()};
        // O tempo de ida e volta é medido aqui, no relógio local, e informado ao
        // servidor. Calcular no servidor mediria descompasso de relógio entre as
        // máquinas, não latência.
        p["rtt"] = Json{ping.load()};

        enviadoEm.store(monotonicoMs());
        if (!socket.enviar(p.paraTexto())) return;

        for (int i = 0; i < 10 && pingando.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void Signaling::Interno::tratar(const std::string& bruto) {
    const Json msg = Json::analisar(bruto);
    if (!msg.ehObjeto()) return;

    const std::string tipo = msg.texto("type");

    if (tipo == "pong") {
        const int64_t ida = monotonicoMs() - enviadoEm.load();
        ping.store(ida < 1 ? 1 : ida);
        return;
    }

    if (tipo == "room-pings") {
        const Json& pings = msg.filho("pings");
        std::lock_guard trava(this->trava);
        for (const auto& [id, valor] : pings.campos()) {
            auto achou = pessoas.find(id);
            if (achou != pessoas.end()) {
                achou->second.pingMs = static_cast<int64_t>(valor.comoNumero());
            }
        }
        return;
    }

    if (tipo == "joined") {
        std::vector<Participante> jaEstavam;
        {
            std::lock_guard trava(this->trava);
            eu = msg.texto("peerId");
            pessoas.clear();
            for (const Json& p : msg.filho("peers").itens()) {
                Participante participante{p.texto("peerId"), p.texto("name"),
                                          static_cast<int64_t>(p.numero("pingMs"))};
                if (participante.id.empty()) continue;
                pessoas[participante.id] = participante;
                jaEstavam.push_back(participante);
            }
        }
        info("entrou na sala como {} ({} ja estavam)", msg.texto("peerId"), jaEstavam.size());
        if (ouvintes.aoSaberDoModo) ouvintes.aoSaberDoModo(msg.booleano("sfu", false));
        if (ouvintes.aoEntrar) ouvintes.aoEntrar(msg.texto("peerId"), jaEstavam);
        return;
    }

    if (tipo == "peer-joined") {
        Participante p{msg.texto("peerId"), msg.texto("name", "Usuario"), 0};
        if (p.id.empty()) return;
        {
            std::lock_guard trava(this->trava);
            pessoas[p.id] = p;
        }
        info("chegou: {} ({})", p.nome, p.id);
        if (ouvintes.aoChegarAlguem) ouvintes.aoChegarAlguem(p);
        return;
    }

    if (tipo == "peer-left") {
        const std::string id = msg.texto("peerId");
        if (id.empty()) return;
        {
            std::lock_guard trava(this->trava);
            pessoas.erase(id);
        }
        info("saiu: {}", id);
        if (ouvintes.aoSairAlguem) ouvintes.aoSairAlguem(id);
        return;
    }

    // O resto é repasse ponto a ponto. O "from" vem carimbado pelo servidor e
    // não dá para o remetente forjar.
    const std::string de = msg.texto("from");
    if (!de.empty() && ouvintes.aoRepasse) ouvintes.aoRepasse(de, msg);
}

}  // namespace gl
