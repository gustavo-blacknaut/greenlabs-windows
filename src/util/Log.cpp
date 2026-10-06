#include "util/Log.h"

#include <windows.h>

#include <shlobj.h>  // SHGetKnownFolderPath
#include <share.h>   // _SH_DENYNO

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
std::wstring g_caminhoW;
size_t g_bytes = 0;
std::string g_ultimaMensagem;
Nivel g_ultimoNivel = Nivel::Info;
std::chrono::steady_clock::time_point g_ultimoRegistro{};
std::chrono::steady_clock::time_point g_ultimoFlush{};

// Um arquivo que cresce para sempre acaba tomando o disco de quem deixa o
// aplicativo aberto o dia inteiro. Passando disto, recomeça.
constexpr size_t kTamanhoMaximo = 512 * 1024;

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
    std::string saida(static_cast<size_t>(tamanho), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, bruto, -1, saida.data(), tamanho, nullptr, nullptr);
    saida.pop_back();
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
        (dados.nFileSizeHigh != 0 || dados.nFileSizeLow >= kTamanhoMaximo)) {
        modo = L"w";
    }

    // _wfsopen com _SH_DENYNO, e não _wfopen_s: o fopen normal abre o arquivo
    // em exclusivo, e aí ninguém consegue ler o log enquanto o programa roda -
    // que é justamente quando ele interessa. Com o compartilhamento liberado dá
    // para acompanhar ao vivo com qualquer visualizador.
    g_arquivo = ::_wfsopen(caminho.c_str(), modo, _SH_DENYNO);
    if (!g_arquivo) return {};

    g_caminho = paraUtf8(caminho.c_str());
    g_caminhoW = caminho;
    g_bytes = modo[0] == L'a' ? dados.nFileSizeLow : 0;
    return g_caminho;
}

bool limparLog() {
    std::lock_guard trava(g_mutex);
    if (!g_arquivo) return false;
    std::fclose(g_arquivo);
    g_arquivo = ::_wfsopen(g_caminhoW.c_str(), L"w", _SH_DENYNO);
    g_bytes = 0;
    g_ultimaMensagem.clear();
    return g_arquivo != nullptr;
}

const std::string& caminhoDoLog() { return g_caminho; }

void escreverLog(Nivel nivel, std::string_view texto) {
    texto = texto.substr(0, 2048);
    const auto agora = std::chrono::floor<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
    const auto linha = std::format("[{:%FT%T}Z]{}{}\n", agora, prefixo(nivel), texto);

    std::lock_guard trava(g_mutex);
    const auto instante = std::chrono::steady_clock::now();
    if (nivel == g_ultimoNivel && texto == g_ultimaMensagem &&
        instante - g_ultimoRegistro < std::chrono::seconds(5)) return;
    g_ultimaMensagem = texto;
    g_ultimoNivel = nivel;
    g_ultimoRegistro = instante;
    std::fwrite(linha.data(), 1, linha.size(), stdout);
    if (nivel == Nivel::Erro) std::fflush(stdout);

    if (g_arquivo) {
        if (g_bytes + linha.size() > kTamanhoMaximo) {
            std::fclose(g_arquivo);
            g_arquivo = ::_wfsopen(g_caminhoW.c_str(), L"w", _SH_DENYNO);
            g_bytes = 0;
            if (!g_arquivo) return;
        }
        std::fwrite(linha.data(), 1, linha.size(), g_arquivo);
        g_bytes += linha.size();
        if (nivel == Nivel::Erro || instante - g_ultimoFlush >= std::chrono::seconds(2)) {
            std::fflush(g_arquivo);
            g_ultimoFlush = instante;
        }
    }
}

std::string hr(long codigo) {
    return std::format("0x{:08X}", static_cast<unsigned long>(codigo));
}

}  // namespace gl
