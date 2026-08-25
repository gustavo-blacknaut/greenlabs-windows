// greenlabs-sinal: entra numa sala de verdade e mostra o que acontece.
//
// Exercita o cliente WebSocket, o JSON e o protocolo contra o servidor que já
// está no ar, sem UI e sem mídia. Quem roda isto deve aparecer no /rooms do
// servidor, com nome e ping.
//
//   greenlabs-sinal --servidor localhost:25640 --sala call1 --nome Teste
//   greenlabs-sinal --servidor localhost:25640 --segundos 20 --eco

#include <windows.h>

#include <objbase.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "network/Signaling.h"
#include "util/Json.h"
#include "util/Log.h"

namespace {

struct Opcoes {
    std::string servidor = "localhost:25640";
    std::string sala = "call1";
    std::string nome = "Nativo";
    uint32_t segundos = 15;
    bool eco = false;  // devolve um "oi" para quem chegar, testando o repasse
};

Opcoes lerOpcoes(int argc, char** argv) {
    Opcoes o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool temProximo = i + 1 < argc;
        if (arg == "--servidor" && temProximo) o.servidor = argv[++i];
        else if (arg == "--sala" && temProximo) o.sala = argv[++i];
        else if (arg == "--nome" && temProximo) o.nome = argv[++i];
        else if (arg == "--segundos" && temProximo) o.segundos = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (arg == "--eco") o.eco = true;
        else if (arg == "-h" || arg == "--help") {
            std::puts(
                "greenlabs-sinal\n"
                "  --servidor host:porta   endereco do servidor (padrao: localhost:25640)\n"
                "  --sala nome             sala a entrar (padrao: call1)\n"
                "  --nome apelido          como aparecer para os outros\n"
                "  --segundos N            quanto tempo ficar (padrao: 15)\n"
                "  --eco                   responde a quem chegar, testando o repasse");
            std::exit(0);
        }
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const Opcoes opcoes = lerOpcoes(argc, argv);

    gl::Signaling sinal;
    std::atomic<uint64_t> repassesRecebidos{0};
    std::atomic<bool> caiu{false};

    gl::Signaling::Ouvintes ouvintes;

    ouvintes.aoEntrar = [&](const std::string& eu, const std::vector<gl::Participante>& pares) {
        std::printf("  meu id: %s\n", eu.c_str());
        if (pares.empty()) {
            std::puts("  sala vazia - sou o primeiro");
        }
        for (const auto& p : pares) {
            std::printf("  ja estava: %-20s %s\n", p.nome.c_str(), p.id.c_str());
        }
        // Quem entra manda a oferta para quem ja estava. Aqui so o cumprimento,
        // porque a midia ainda nao existe - mas o caminho e o mesmo.
        if (opcoes.eco) {
            for (const auto& p : pares) {
                gl::Json oi = gl::Json::objeto();
                oi["type"] = gl::Json{"stream-meta"};
                oi["streamId"] = gl::Json{"teste-nativo"};
                oi["name"] = gl::Json{"cliente nativo em C++"};
                oi["kind"] = gl::Json{"screen"};
                sinal.enviarPara(p.id, oi);
            }
        }
    };

    ouvintes.aoChegarAlguem = [&](const gl::Participante& p) {
        if (!opcoes.eco) return;
        gl::Json oi = gl::Json::objeto();
        oi["type"] = gl::Json{"stream-meta"};
        oi["streamId"] = gl::Json{"teste-nativo"};
        oi["name"] = gl::Json{"cliente nativo em C++"};
        oi["kind"] = gl::Json{"screen"};
        sinal.enviarPara(p.id, oi);
    };

    ouvintes.aoRepasse = [&](const std::string& de, const gl::Json& msg) {
        repassesRecebidos.fetch_add(1);
        std::printf("  repasse de %s: type=%s%s\n", de.substr(0, 8).c_str(),
                    msg.texto("type").c_str(),
                    msg.tem("name") ? (" name=" + msg.texto("name")).c_str() : "");
    };

    ouvintes.aoCair = [&](const std::string& motivo) {
        caiu.store(true);
        gl::aviso("a conexao caiu: {}", motivo);
    };

    sinal.definirOuvintes(std::move(ouvintes));

    if (!sinal.entrar(opcoes.servidor, opcoes.sala, opcoes.nome)) {
        gl::erro("nao consegui entrar na sala");
        ::CoUninitialize();
        return 1;
    }

    gl::info("ficando na sala '{}' por {}s...", opcoes.sala, opcoes.segundos);
    const auto fim = std::chrono::steady_clock::now() + std::chrono::seconds(opcoes.segundos);
    while (std::chrono::steady_clock::now() < fim && !caiu.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::puts("");
    std::puts("========================= RESULTADO =========================");
    std::printf("conectado ate o fim : %s\n", caiu.load() ? "NAO (caiu)" : "sim");
    std::printf("meu id              : %s\n", sinal.meuId().c_str());
    std::printf("ping                : %lld ms\n", (long long)sinal.pingMs());
    std::printf("repasses recebidos  : %llu\n", (unsigned long long)repassesRecebidos.load());
    std::puts("participantes na sala:");
    for (const auto& p : sinal.participantes()) {
        std::printf("  %-20s %s  (%lld ms)\n", p.nome.c_str(), p.id.c_str(), (long long)p.pingMs);
    }
    if (sinal.participantes().empty()) std::puts("  (ninguem alem de mim)");
    std::puts("============================================================");

    sinal.sair();
    ::CoUninitialize();
    return caiu.load() ? 1 : 0;
}
