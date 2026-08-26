#pragma once

// Log simples e sem dependência. O formato copia o do servidor em Go e o do
// AudioCapture.cs — carimbo ISO na frente — para as três coisas poderem ser
// lidas juntas quando algo dá errado numa chamada.

#include <cstdio>
#include <format>
#include <string_view>

namespace gl {

enum class Nivel { Info, Aviso, Erro };

void escreverLog(Nivel nivel, std::string_view texto);

// Passa a gravar também num arquivo, além do stdout.
//
// Precisa existir porque o aplicativo é WinMain: não tem console, então tudo
// que ia para o stdout se perdia. Quando alguma coisa falhava no início, a
// única informação que sobrava era a caixa de erro — que não sabe dizer o que
// aconteceu de verdade.
//
// Devolve o caminho do arquivo, ou vazio se não deu para abrir. As ferramentas
// de console não chamam: para elas o stdout já resolve.
std::string abrirArquivoDeLog();

// Caminho do arquivo em uso, vazio enquanto ninguém chamou abrirArquivoDeLog.
const std::string& caminhoDoLog();

template <class... Args>
void info(std::format_string<Args...> f, Args&&... args) {
    escreverLog(Nivel::Info, std::format(f, std::forward<Args>(args)...));
}

template <class... Args>
void aviso(std::format_string<Args...> f, Args&&... args) {
    escreverLog(Nivel::Aviso, std::format(f, std::forward<Args>(args)...));
}

template <class... Args>
void erro(std::format_string<Args...> f, Args&&... args) {
    escreverLog(Nivel::Erro, std::format(f, std::forward<Args>(args)...));
}

// Formata um HRESULT como 0x80070005, que é como ele aparece na documentação
// da Microsoft e nos logs do capturador antigo.
std::string hr(long codigo);

}  // namespace gl
