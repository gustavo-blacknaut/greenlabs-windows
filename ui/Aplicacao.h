#pragma once

// A aplicação: janela, laço de eventos e as telas.
//
// A interface não conhece rede nem captura por dentro — ela lê um estado e
// dispara ações. É o que permite o núcleo rodar sem janela nenhuma (o
// greenlabs-probe e o greenlabs-sinal fazem exatamente isso).

#include <memory>
#include <string>

namespace gl {

class Aplicacao {
public:
    Aplicacao();
    ~Aplicacao();

    Aplicacao(const Aplicacao&) = delete;
    Aplicacao& operator=(const Aplicacao&) = delete;

    // Preencher servidor/sala/nome faz o aplicativo ja entrar ao abrir. Serve
    // para atalho de area de trabalho e para poder testar sem digitar.
    struct Inicial {
        std::string servidor;
        std::string sala;
        std::string nome;
        bool transmitirJa = false;
    };

    bool iniciar(const std::wstring& titulo, int largura, int altura, const Inicial& inicial = {});
    int rodar();

    // O que impediu iniciar() de terminar, em texto para mostrar ao usuário.
    //
    // Existe porque um bool só faz toda falha virar a mesma mensagem, e a
    // mensagem acaba chutando a causa. Quem lê precisa saber se foi a janela,
    // o monitor ou a placa — chutar manda a pessoa investigar o lugar errado.
    const std::wstring& motivoDaFalha() const;

    // Publico so para o procedimento de janela do Win32 alcancar: ele e uma
    // funcao livre, exigencia da API, e precisa chegar ao estado.
    struct Interno;

private:
    std::unique_ptr<Interno> d_;
};

}  // namespace gl
