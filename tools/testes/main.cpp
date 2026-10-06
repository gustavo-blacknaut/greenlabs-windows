#include "audio/FilaAudio.h"
#include "audio/RingBuffer.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

void verificar(bool condicao) {
    if (!condicao) { std::cerr << "Falha na fila de audio\n"; std::exit(1); }
}

int main() {
    gl::FilaAudio fila;
    gl::FilaAudio::Pacote pacote;
    uint64_t dados = 1;
    verificar(!fila.retirar(pacote));
    verificar(!fila.enfileirar(nullptr, 1, 0));
    verificar(!fila.enfileirar(reinterpret_cast<uint8_t*>(&dados), 1501, 0));
    for (int i = 0; i < 32; ++i) verificar(fila.enfileirar(reinterpret_cast<uint8_t*>(&dados), sizeof(dados), i));
    verificar(!fila.enfileirar(reinterpret_cast<uint8_t*>(&dados), sizeof(dados), 32));
    verificar(fila.retirar(pacote) && pacote.tempoUs == 31);
    verificar(!fila.retirar(pacote));
    fila.limpar();

    std::atomic<bool> terminou{false};
    std::thread produtor([&] {
        for (uint64_t i = 1; i <= 100000; ++i) {
            uint64_t conteudo[2]{i, ~i};
            while (!fila.enfileirar(reinterpret_cast<uint8_t*>(conteudo), sizeof(conteudo), static_cast<int64_t>(i))) std::this_thread::yield();
        }
        terminou.store(true, std::memory_order_release);
    });
    uint64_t ultimo = 0;
    for (;;) {
        // Testa a fila depois de observar o termino: nenhum pacote final fica
        // invisivel por uma leitura anterior do indice de escrita.
        const bool fim = terminou.load(std::memory_order_acquire);
        if (!fila.retirar(pacote)) { if (fim) break; std::this_thread::yield(); continue; }
        uint64_t conteudo[2]{};
        std::memcpy(conteudo, pacote.dados.data(), sizeof(conteudo));
        verificar(pacote.tamanho == sizeof(conteudo) && conteudo[0] > ultimo && conteudo[1] == ~conteudo[0]);
        verificar(pacote.tempoUs == static_cast<int64_t>(conteudo[0]));
        ultimo = conteudo[0];
    }
    produtor.join();
    verificar(ultimo == 100000);
    gl::RingBuffer pcm(48000, 2);
    float entrada[4]{1,2,3,4}, saida[6]{};
    pcm.escrever(entrada, 2);
    verificar(pcm.ler(saida, 6) == 4 && saida[0] == 1 && saida[3] == 4 && saida[4] == 0);
    verificar(pcm.ocupacao() == 0);
    std::cout << "Fila de audio: limites, descarte e concorrencia OK\n";
}
