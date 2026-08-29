#include "network/Pacer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace gl {
namespace {

// O pacer espalha um pouco mais rápido do que o encoder produz. Espalhar na
// taxa exata faria cada quadro acima da média atrasar o seguinte.
constexpr double kFolga = 1.5;

// Teto do espalhamento por chamada.
//
// Isto é o que impede o remédio de virar doença: quem chama aqui é a thread da
// interface, a mesma que captura e desenha. A 30 quadros por segundo há 33 ms
// de orçamento no total, e 10 ms consumiam quase um terço dele - a captura caía
// junto. 5 ms tira o pico da rajada e deixa o resto do orçamento em paz.
constexpr auto kMaximoEspalhado = std::chrono::milliseconds(5);

// Abaixo disto não há rajada que valha espalhar. Um quadro P comum cabe em
// poucos pacotes e sai sem incomodar ninguém; o que entope a fila do roteador é
// o quadro-chave, com dezenas deles. Espalhar os pequenos só somaria atraso a
// cada quadro sem tirar carga nenhuma da rede.
constexpr size_t kMinimoParaEspalhar = 16;

// Menor pausa que vale a pena pedir ao sistema. Abaixo disso o próprio
// escalonador do Windows já erra mais que o intervalo.
constexpr auto kPasso = std::chrono::milliseconds(1);

}  // namespace

struct Pacer::Interno {
    std::atomic<uint32_t> taxaBits{4'000'000};
    std::atomic<uint64_t> perdidos{0};
    std::atomic<size_t> ultimoLote{0};
};

Pacer::Pacer(uint32_t taxaBits) : d_(std::make_unique<Interno>()) {
    if (taxaBits > 0) d_->taxaBits.store(taxaBits);
}

Pacer::~Pacer() = default;

void Pacer::ajustarTaxa(uint32_t taxaBits) {
    if (taxaBits > 0) d_->taxaBits.store(taxaBits);
}

size_t Pacer::naFila() const { return d_->ultimoLote.load(std::memory_order_relaxed); }

uint64_t Pacer::descartes() const { return d_->perdidos.load(std::memory_order_relaxed); }

// Tudo acontece dentro desta chamada, de propósito.
//
// A primeira versão tinha uma thread própria que guardava os pacotes e chamava
// `enviar` mais tarde. Funcionava até a conexão ser desmontada: aí o callback
// que ela guardara apontava para um transporte que já não existia, e o programa
// morria com violação de acesso - ou travava, quando a thread era destruída de
// dentro dela mesma.
//
// Espalhar aqui dentro custa alguns milissegundos da thread que chamou, e em
// troca não há nada guardado entre chamadas: enquanto este código roda, a
// cadeia inteira está viva porque foi ela que nos chamou. É mais simples e não
// tem como apontar para o que já morreu.
void Pacer::outgoing(rtc::message_vector& mensagens, const rtc::message_callback& enviar) {
    if (mensagens.empty() || !enviar) return;

    rtc::message_vector controle;
    rtc::message_vector midia;
    size_t bytes = 0;

    for (auto& m : mensagens) {
        if (!m) continue;
        // RTCP não espera: é controle, é pequeno, e atrasá-lo só faria o outro
        // lado demorar para reagir ao que já deu errado.
        if (m->type == rtc::Message::Control) {
            controle.push_back(std::move(m));
        } else {
            bytes += m->size();
            midia.push_back(std::move(m));
        }
    }

    mensagens = std::move(controle);
    if (midia.empty()) return;

    d_->ultimoLote.store(bytes, std::memory_order_relaxed);

    // Lote pequeno sai inteiro: não é rajada, e atrasá-lo seria atraso puro.
    if (midia.size() < kMinimoParaEspalhar) {
        for (auto& m : midia) enviar(std::move(m));
        return;
    }

    const double porSegundo = static_cast<double>(d_->taxaBits.load()) / 8.0 * kFolga;
    const auto ideal = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double>(static_cast<double>(bytes) / porSegundo));
    const auto espalhar = std::min(ideal, kMaximoEspalhado);

    if (espalhar < kPasso) {
        for (auto& m : midia) enviar(std::move(m));
        return;
    }

    // Divide o lote em fatias de um milissegundo. É o que transforma "85
    // pacotes no mesmo instante" - que entope a fila do roteador e derruba o
    // ping de tudo na casa - em "85 pacotes ao longo de 10 ms", que a rede
    // absorve sem notar.
    const size_t passos = static_cast<size_t>(espalhar / kPasso);
    const size_t porPasso = (midia.size() + passos - 1) / passos;

    size_t i = 0;
    while (i < midia.size()) {
        const size_t ate = std::min(i + porPasso, midia.size());
        for (; i < ate; ++i) enviar(std::move(midia[i]));
        if (i < midia.size()) std::this_thread::sleep_for(kPasso);
    }
}

}  // namespace gl
