#include "util/Log.h"

#include <chrono>
#include <mutex>

namespace gl {
namespace {

// stdout não é atômico entre threads: sem isto, captura de áudio e de vídeo
// escrevendo ao mesmo tempo produzem linhas embaralhadas.
std::mutex g_mutex;

const char* prefixo(Nivel n) {
    switch (n) {
        case Nivel::Aviso: return " AVISO ";
        case Nivel::Erro:  return " ERRO  ";
        default:           return "       ";
    }
}

}  // namespace

void escreverLog(Nivel nivel, std::string_view texto) {
    const auto agora = std::chrono::floor<std::chrono::milliseconds>(
        std::chrono::system_clock::now());

    std::lock_guard trava(g_mutex);
    std::fwrite("[", 1, 1, stdout);
    const auto carimbo = std::format("{:%FT%T}Z]{}", agora, prefixo(nivel));
    std::fwrite(carimbo.data(), 1, carimbo.size(), stdout);
    std::fwrite(texto.data(), 1, texto.size(), stdout);
    std::fwrite("\n", 1, 1, stdout);
    std::fflush(stdout);
}

std::string hr(long codigo) {
    return std::format("0x{:08X}", static_cast<unsigned long>(codigo));
}

}  // namespace gl
