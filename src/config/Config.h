#pragma once

// O que fica guardado entre uma execução e outra.
//
// Vai para %APPDATA%\GreenLabs\config.json — ninguém quer digitar o endereço do
// servidor toda vez que abre o aplicativo. Só preferências: nada de senha,
// nada de identificador, porque o GreenLabs não tem conta.

#include <cstdint>
#include <string>
#include <vector>

namespace gl {

struct Config {
    std::string servidor;
    std::string sala = "call1";
    std::string nome;
    int qualidade = 1;  // índice na tabela de qualidades
    int monitor = 0;

    // Servidores já usados, do mais recente para o mais antigo. Quem entra numa
    // sala hoje volta nela amanhã, e digitar o endereço de novo toda vez é o
    // tipo de atrito que faz a pessoa desistir.
    std::vector<std::string> servidores;

    // Põe um servidor no topo da lista, sem repetir. Guarda no máximo seis:
    // além disso vira lista de histórico, não de atalho.
    void lembrarServidor(const std::string& endereco);

    // Devolve o padrão quando o arquivo não existe ou está corrompido: perder a
    // preferência é chato, travar a abertura do aplicativo é pior.
    static Config carregar();
    void salvar() const;

    static std::string caminho();
};

}  // namespace gl
