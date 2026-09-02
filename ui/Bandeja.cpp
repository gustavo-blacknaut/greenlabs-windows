#include "ui/Bandeja.h"

#include <shellapi.h>

#include "util/Log.h"

namespace gl {

namespace {
constexpr UINT kIdDoIcone = 1;
constexpr UINT kMenuAbrir = 40001;
constexpr UINT kMenuSair = 40002;
}  // namespace

Bandeja::Bandeja() = default;

Bandeja::~Bandeja() { remover(); }

bool Bandeja::criar(HWND janela, UINT mensagem, const wchar_t* dica) {
    if (criado_) return true;

    dados_ = {};
    dados_.cbSize = sizeof(dados_);
    dados_.hWnd = janela;
    dados_.uID = kIdDoIcone;
    dados_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    dados_.uCallbackMessage = mensagem;

    // O mesmo ícone que está dentro do .exe. Carregado por LoadIconW com o
    // recurso 1, que é o que o recursos.rc define.
    dados_.hIcon = ::LoadIconW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (!dados_.hIcon) {
        // Sem o nosso, o do sistema: um ícone genérico é melhor do que ícone
        // nenhum, que no Windows vira um retângulo branco irreconhecível.
        dados_.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    }

    ::lstrcpynW(dados_.szTip, dica, ARRAYSIZE(dados_.szTip));

    criado_ = ::Shell_NotifyIconW(NIM_ADD, &dados_) != FALSE;
    if (!criado_) {
        erro("nao consegui criar o icone da bandeja");
    }
    return criado_;
}

void Bandeja::remover() {
    if (!criado_) return;
    ::Shell_NotifyIconW(NIM_DELETE, &dados_);
    criado_ = false;
}

void Bandeja::avisar(const wchar_t* titulo, const wchar_t* texto) {
    if (!criado_) return;

    NOTIFYICONDATAW balao = dados_;
    balao.uFlags = NIF_INFO;
    balao.dwInfoFlags = NIIF_NONE;
    ::lstrcpynW(balao.szInfoTitle, titulo, ARRAYSIZE(balao.szInfoTitle));
    ::lstrcpynW(balao.szInfo, texto, ARRAYSIZE(balao.szInfo));
    ::Shell_NotifyIconW(NIM_MODIFY, &balao);
}

bool Bandeja::tratar(HWND janela, WPARAM w, LPARAM l) {
    if (LOWORD(w) != kIdDoIcone) return false;

    switch (LOWORD(l)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            if (aoAbrir) aoAbrir();
            return true;

        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: {
            HMENU menu = ::CreatePopupMenu();
            if (!menu) return true;

            ::AppendMenuW(menu, MF_STRING, kMenuAbrir, L"Abrir GreenLabs");
            ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            ::AppendMenuW(menu, MF_STRING, kMenuSair, L"Sair");

            POINT p{};
            ::GetCursorPos(&p);

            // Sem isto o menu não fecha ao clicar fora dele. É exigência
            // documentada de menu de bandeja, e sem ela o menu fica preso na
            // tela até a pessoa escolher algo.
            ::SetForegroundWindow(janela);

            const int escolha = ::TrackPopupMenu(
                menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0, janela, nullptr);
            ::DestroyMenu(menu);

            if (escolha == kMenuAbrir && aoAbrir) aoAbrir();
            if (escolha == kMenuSair && aoSair) aoSair();
            return true;
        }

        default:
            return false;
    }
}

}  // namespace gl
