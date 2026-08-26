// GreenLabs — cliente nativo para Windows.
//
// Sem Electron, sem Chromium, sem Node.

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>  // CommandLineToArgvW

#include <string>
#include <vector>

#include "ui/Aplicacao.h"
#include "util/Log.h"

namespace {

// A linha de comando serve para atalho de area de trabalho ("abrir ja na sala
// do grupo") e para conferir o aplicativo sem digitar nada.
gl::Aplicacao::Inicial lerLinhaDeComando() {
    gl::Aplicacao::Inicial inicial;

    int total = 0;
    LPWSTR* partes = ::CommandLineToArgvW(::GetCommandLineW(), &total);
    if (!partes) return inicial;

    auto paraUtf8 = [](LPCWSTR w) {
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        std::string saida(n > 1 ? static_cast<size_t>(n - 1) : 0, 0);
        if (n > 1) ::WideCharToMultiByte(CP_UTF8, 0, w, -1, saida.data(), n, nullptr, nullptr);
        return saida;
    };

    for (int i = 1; i < total; ++i) {
        const std::wstring arg = partes[i];
        const bool temProximo = i + 1 < total;
        if (arg == L"--servidor" && temProximo) inicial.servidor = paraUtf8(partes[++i]);
        else if (arg == L"--sala" && temProximo) inicial.sala = paraUtf8(partes[++i]);
        else if (arg == L"--nome" && temProximo) inicial.nome = paraUtf8(partes[++i]);
        else if (arg == L"--transmitir") inicial.transmitirJa = true;
    }
    ::LocalFree(partes);
    return inicial;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    // Antes de qualquer coisa que possa falhar: sem console, o log só existe se
    // o arquivo estiver aberto, e a falha que mais importa é a do início.
    const std::string log = gl::abrirArquivoDeLog();
    gl::info("GreenLabs iniciando");
    if (!log.empty()) gl::info("log em {}", log);

    // MULTITHREADED porque a captura de áudio, o encoder e a rede tocam em COM
    // a partir das próprias threads.
    const HRESULT inicio = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(inicio)) {
        ::MessageBoxW(nullptr, L"Nao foi possivel inicializar o COM.", L"GreenLabs", MB_ICONERROR);
        return 1;
    }

    gl::Aplicacao app;
    if (!app.iniciar(L"GreenLabs", 1320, 820, lerLinhaDeComando())) {
        // A mensagem antiga chutava "monitor desconectado ou placa sem Direct3D
        // 11" para qualquer falha, inclusive as que nada tinham a ver com isso.
        // Quem lia ia investigar a placa de video sem motivo.
        std::wstring texto = L"Nao foi possivel iniciar.";
        if (!app.motivoDaFalha().empty()) texto += L"\n\n" + app.motivoDaFalha();

        // O caminho vem do log de verdade, não de um palpite: quem abrir a
        // caixa consegue ir direto no arquivo.
        if (!log.empty()) {
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, log.c_str(), -1, nullptr, 0);
            std::wstring caminho(n > 1 ? static_cast<size_t>(n - 1) : 0, 0);
            if (n > 1) ::MultiByteToWideChar(CP_UTF8, 0, log.c_str(), -1, caminho.data(), n);
            texto += L"\n\nDetalhes em:\n" + caminho;
        }

        gl::erro("falha ao iniciar");
        ::MessageBoxW(nullptr, texto.c_str(), L"GreenLabs", MB_ICONERROR);
        ::CoUninitialize();
        return 1;
    }

    const int codigo = app.rodar();
    ::CoUninitialize();
    return codigo;
}
