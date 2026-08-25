#include "capture/ProcessTree.h"

#include <windows.h>

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "util/Log.h"

using Microsoft::WRL::ComPtr;

namespace gl {
namespace {

std::string paraMinusculoSemExe(const wchar_t* bruto) {
    std::string saida;
    saida.reserve(32);
    for (const wchar_t* p = bruto; *p; ++p) {
        saida.push_back(static_cast<char>(*p < 128 ? ::tolower(static_cast<int>(*p)) : '?'));
    }
    if (saida.size() > 4 && saida.compare(saida.size() - 4, 4, ".exe") == 0) {
        saida.resize(saida.size() - 4);
    }
    return saida;
}

}  // namespace

std::vector<ProcessoNomeado> listarProcessos() {
    std::vector<ProcessoNomeado> lista;

    HANDLE foto = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (foto == INVALID_HANDLE_VALUE) {
        aviso("nao foi possivel listar processos: {}", ::GetLastError());
        return lista;
    }

    PROCESSENTRY32W entrada{};
    entrada.dwSize = sizeof(entrada);
    if (::Process32FirstW(foto, &entrada)) {
        do {
            lista.push_back(ProcessoNomeado{
                entrada.th32ProcessID,
                entrada.th32ParentProcessID,
                paraMinusculoSemExe(entrada.szExeFile),
            });
        } while (::Process32NextW(foto, &entrada));
    }
    ::CloseHandle(foto);
    return lista;
}

std::vector<uint32_t> pidsComSessaoDeAudio() {
    std::vector<uint32_t> pids;

    ComPtr<IMMDeviceEnumerator> enumerador;
    HRESULT resultado = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                           CLSCTX_ALL, IID_PPV_ARGS(&enumerador));
    if (FAILED(resultado)) {
        aviso("MMDeviceEnumerator falhou: {}", hr(resultado));
        return pids;
    }

    ComPtr<IMMDevice> saida;
    resultado = enumerador->GetDefaultAudioEndpoint(eRender, eConsole, &saida);
    if (FAILED(resultado)) return pids;

    ComPtr<IAudioSessionManager2> gerente;
    resultado = saida->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &gerente);
    if (FAILED(resultado)) return pids;

    ComPtr<IAudioSessionEnumerator> sessoes;
    if (FAILED(gerente->GetSessionEnumerator(&sessoes))) return pids;

    int total = 0;
    if (FAILED(sessoes->GetCount(&total))) return pids;

    for (int i = 0; i < total; ++i) {
        ComPtr<IAudioSessionControl> controle;
        if (FAILED(sessoes->GetSession(i, &controle))) continue;

        ComPtr<IAudioSessionControl2> controle2;
        if (FAILED(controle.As(&controle2))) continue;

        DWORD pid = 0;
        if (SUCCEEDED(controle2->GetProcessId(&pid)) && pid != 0) {
            pids.push_back(pid);
        }
    }
    return pids;
}

uint32_t acharRaizParaExcluir(const std::vector<std::string>& nomes) {
    const auto processos = listarProcessos();

    std::unordered_set<uint32_t> casaram;
    std::unordered_map<uint32_t, uint32_t> pais;
    pais.reserve(processos.size());

    for (const auto& proc : processos) {
        pais[proc.pid] = proc.ppid;
        for (const auto& alvo : nomes) {
            if (!alvo.empty() && proc.nome.find(alvo) != std::string::npos) {
                casaram.insert(proc.pid);
                break;
            }
        }
    }
    if (casaram.empty()) return 0;

    // Prefere o processo que realmente tem sessão de áudio: num app com vários
    // processos, é ele que está tocando.
    uint32_t semente = 0;
    for (uint32_t pid : pidsComSessaoDeAudio()) {
        if (casaram.count(pid)) {
            semente = pid;
            break;
        }
    }
    if (semente == 0) semente = *casaram.begin();

    uint32_t raiz = semente;
    std::unordered_set<uint32_t> visitados;
    while (visitados.insert(raiz).second) {
        auto achou = pais.find(raiz);
        if (achou == pais.end()) break;
        const uint32_t pai = achou->second;
        // A guarda que impede subir para fora do app. Ver o comentário do .h.
        if (!casaram.count(pai)) break;
        raiz = pai;
    }

    info("[exclude] sessao de audio pid {} ({}) -> raiz da arvore {} ({})",
         semente, nomeDoProcesso(semente), raiz, nomeDoProcesso(raiz));
    return raiz;
}

std::string nomeDoProcesso(uint32_t pid) {
    for (const auto& proc : listarProcessos()) {
        if (proc.pid == pid) return proc.nome;
    }
    return "?";
}

}  // namespace gl
