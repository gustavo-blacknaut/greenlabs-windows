#pragma once

// O ícone ao lado do relógio, e o que ele faz.
//
// Fechar a janela não fecha o aplicativo: ele some para a bandeja e a
// transmissão continua. Esse é o comportamento que qualquer um espera de um
// programa de chamada - fechar a janela do Discord no meio de uma conversa não
// desliga o microfone - e é o que o cliente em Electron já fazia. Aqui a janela
// fechava de verdade e a transmissão morria junto, no meio da chamada.
//
// A bandeja passa a ser a única forma de trazer a janela de volta ou de sair de
// verdade, então ela precisa existir ANTES de a janela poder ser escondida.

#include <windows.h>

#include <shellapi.h>  // NOTIFYICONDATAW: WIN32_LEAN_AND_MEAN tira do windows.h

#include <functional>

namespace gl {

class Bandeja {
public:
    Bandeja();
    ~Bandeja();

    Bandeja(const Bandeja&) = delete;
    Bandeja& operator=(const Bandeja&) = delete;

    // Cria o ícone. `mensagem` é a mensagem de janela que os eventos do mouse
    // sobre o ícone vão gerar - quem chama repassa para tratar().
    bool criar(HWND janela, UINT mensagem, const wchar_t* dica);

    // Tira o ícone. Chamar sempre antes de sair: ícone órfão fica na bandeja
    // até alguém passar o mouse por cima, e o Windows não limpa sozinho.
    void remover();

    // Trata um evento vindo do ícone. Devolve true quando consumiu.
    //
    // Clique esquerdo restaura a janela; clique direito abre o menu com
    // "Abrir" e "Sair".
    bool tratar(HWND janela, WPARAM w, LPARAM l);

    // O que fazer quando o menu pede cada coisa.
    std::function<void()> aoAbrir;
    std::function<void()> aoSair;

    // Um balão do sistema. Usado uma vez só: na primeira vez que a janela é
    // fechada, para a pessoa saber que o programa continua vivo em vez de achar
    // que ele travou.
    void avisar(const wchar_t* titulo, const wchar_t* texto);

    bool existe() const { return criado_; }

private:
    NOTIFYICONDATAW dados_{};
    bool criado_ = false;
};

}  // namespace gl
