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
    // MULTITHREADED porque a captura de áudio, o encoder e a rede tocam em COM
    // a partir das próprias threads.
    const HRESULT inicio = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(inicio)) {
        ::MessageBoxW(nullptr, L"Nao foi possivel inicializar o COM.", L"GreenLabs", MB_ICONERROR);
        return 1;
    }

    gl::Aplicacao app;
    if (!app.iniciar(L"GreenLabs", 1320, 820, lerLinhaDeComando())) {
        ::MessageBoxW(nullptr,
                      L"Nao foi possivel iniciar.\n\n"
                      L"Verifique se ha um monitor conectado e se a placa de video suporta "
                      L"Direct3D 11.",
                      L"GreenLabs", MB_ICONERROR);
        ::CoUninitialize();
        return 1;
    }

    const int codigo = app.rodar();
    ::CoUninitialize();
    return codigo;
}
