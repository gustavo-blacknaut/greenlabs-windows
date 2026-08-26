#pragma once

// O que fica guardado entre uma execução e outra.
//
// Vai para %APPDATA%\GreenLabs\config.json — ninguém quer digitar o endereço do
// servidor toda vez que abre o aplicativo. Só preferências: nada de senha,
// nada de identificador, porque o GreenLabs não tem conta.

#include <cstdint>
#include <string>

namespace gl {

struct Config {
    std::string servidor;
    std::string sala = "call1";
    std::string nome;
    int qualidade = 1;  // índice na tabela de qualidades
    int monitor = 0;

    // Devolve o padrão quando o arquivo não existe ou está corrompido: perder a
    // preferência é chato, travar a abertura do aplicativo é pior.
    static Config carregar();
    void salvar() const;

    static std::string caminho();
};

}  // namespace gl
