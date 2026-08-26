#include "util/Log.h"

#include <windows.h>

#include <shlobj.h>  // SHGetKnownFolderPath

#include <chrono>
#include <mutex>
#include <string>

namespace gl {
namespace {

// stdout não é atômico entre threads: sem isto, captura de áudio e de vídeo
// escrevendo ao mesmo tempo produzem linhas embaralhadas.
std::mutex g_mutex;

// Aberto por abrirArquivoDeLog. Protegido pelo mesmo mutex das escritas.
std::FILE* g_arquivo = nullptr;
std::string g_caminho;

// Um arquivo que cresce para sempre acaba tomando o disco de quem deixa o
// aplicativo aberto o dia inteiro. Passando disto, recomeça.
constexpr long kTamanhoMaximo = 4L * 1024 * 1024;

const char* prefixo(Nivel n) {
    switch (n) {
        case Nivel::Aviso: return " AVISO ";
        case Nivel::Erro:  return " ERRO  ";
        default:           return "       ";
    }
}

std::string paraUtf8(const wchar_t* bruto) {
    if (!bruto || !*bruto) return {};
    const int tamanho = ::WideCharToMultiByte(CP_UTF8, 0, bruto, -1, nullptr, 0, nullptr, nullptr);
    if (tamanho <= 1) return {};
    std::string saida(static_cast<size_t>(tamanho - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, bruto, -1, saida.data(), tamanho, nullptr, nullptr);
    return saida;
}

}  // namespace

std::string abrirArquivoDeLog() {
    std::lock_guard trava(g_mutex);
    if (g_arquivo) return g_caminho;

    PWSTR pasta = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &pasta))) return {};

    std::wstring diretorio = std::wstring(pasta) + L"\\GreenLabs";
    ::CoTaskMemFree(pasta);

    // Já existir não é erro: só o que importa é poder escrever depois.
    ::CreateDirectoryW(diretorio.c_str(), nullptr);

    const std::wstring caminho = diretorio + L"\\greenlabs.log";

    // "a" e não "w": fechar e reabrir o aplicativo não pode apagar o registro
    // da sessão que acabou de dar errado, que é justamente a que interessa.
    const wchar_t* modo = L"a";
    WIN32_FILE_ATTRIBUTE_DATA dados{};
    if (::GetFileAttributesExW(caminho.c_str(), GetFileExInfoStandard, &dados) &&
        dados.nFileSizeHigh == 0 && dados.nFileSizeLow > kTamanhoMaximo) {
        modo = L"w";
    }

    if (::_wfopen_s(&g_arquivo, caminho.c_str(), modo) != 0 || !g_arquivo) {
        g_arquivo = nullptr;
        return {};
    }

    g_caminho = paraUtf8(caminho.c_str());
    return g_caminho;
}

const std::string& caminhoDoLog() { return g_caminho; }

void escreverLog(Nivel nivel, std::string_view texto) {
    const auto agora = std::chrono::floor<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
    const auto linha = std::format("[{:%FT%T}Z]{}{}\n", agora, prefixo(nivel), texto);

    std::lock_guard trava(g_mutex);
    std::fwrite(linha.data(), 1, linha.size(), stdout);
    std::fflush(stdout);

    if (g_arquivo) {
        std::fwrite(linha.data(), 1, linha.size(), g_arquivo);
        // Sem flush, uma queda leva junto exatamente as últimas linhas — as que
        // dizem o que aconteceu.
        std::fflush(g_arquivo);
    }
}

std::string hr(long codigo) {
    return std::format("0x{:08X}", static_cast<unsigned long>(codigo));
}

}  // namespace gl
