#pragma once

// Captura do áudio do sistema com um aplicativo de fora.
//
// Usa WASAPI process loopback em modo EXCLUDE: pega tudo que está tocando na
// máquina — jogo, Spotify, navegador, sons do sistema — MENOS a árvore de
// processos indicada. O aplicativo excluído continua tocando normalmente nos
// alto-falantes de quem transmite; ele só não entra na captura.
//
// É exclusão na origem, não mute. Ninguém fica sem ouvir nada.
//
// Porte de electron/AudioCapture.cs do repositório em Electron. O que sumiu na
// tradução: o servidor HTTP local (aqui o consumidor recebe direto), o handler
// COM montado à mão (o WRL resolve) e as consultas WMI (Toolhelp32 é mais
// barato).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gl {

struct FormatoAudio {
    uint32_t taxaAmostragem = 48000;
    uint32_t canais = 2;
};

class AudioCapture {
public:
    // Chamado na thread de áudio, com o buffer do WASAPI ainda em mãos: os
    // dados são float32 intercalados (L,R,L,R...) e valem só durante a chamada.
    //
    // Roda em thread de tempo real. Não alocar, não travar, não fazer E/S aqui:
    // segurar essa chamada estoura o buffer e sai como estalo no áudio.
    using Consumidor = std::function<void(const float* intercalado, uint32_t quadros)>;

    AudioCapture();
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // pidExcluir é a raiz da árvore a deixar de fora (ver ProcessTree.h).
    // Passar 0 captura tudo, sem exclusão nenhuma.
    bool iniciar(uint32_t pidExcluir, Consumidor consumidor);
    void parar();

    bool ativo() const;
    FormatoAudio formato() const;
    uint32_t pidExcluido() const;

    // Quadros descartados pelo WASAPI por não termos lido a tempo. Deve ficar
    // em zero; qualquer número aqui é sinal de consumidor lento.
    uint64_t quadrosComFalha() const;

private:
    struct Interno;
    std::unique_ptr<Interno> d_;
};

}  // namespace gl
