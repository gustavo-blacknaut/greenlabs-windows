#include "capture/ScreenCapture.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "util/Log.h"

using Microsoft::WRL::ComPtr;

namespace gl {
namespace {

std::string paraUtf8(const wchar_t* bruto) {
    if (!bruto || !*bruto) return {};
    const int tamanho = ::WideCharToMultiByte(CP_UTF8, 0, bruto, -1, nullptr, 0, nullptr, nullptr);
    if (tamanho <= 1) return {};
    std::string saida(static_cast<size_t>(tamanho - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, bruto, -1, saida.data(), tamanho, nullptr, nullptr);
    return saida;
}

int64_t frequenciaQpc() {
    static const int64_t f = [] {
        LARGE_INTEGER v{};
        ::QueryPerformanceFrequency(&v);
        return v.QuadPart;
    }();
    return f;
}

int64_t agoraQpc() {
    LARGE_INTEGER v{};
    ::QueryPerformanceCounter(&v);
    return v.QuadPart;
}

// Percorre adaptadores e saídas chamando a função para cada par encontrado.
template <class Fn>
void paraCadaSaida(Fn&& fn) {
    ComPtr<IDXGIFactory1> fabrica;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&fabrica)))) return;

    for (UINT ia = 0;; ++ia) {
        ComPtr<IDXGIAdapter1> adaptador;
        if (fabrica->EnumAdapters1(ia, &adaptador) == DXGI_ERROR_NOT_FOUND) break;

        for (UINT is = 0;; ++is) {
            ComPtr<IDXGIOutput> saida;
            if (adaptador->EnumOutputs(is, &saida) == DXGI_ERROR_NOT_FOUND) break;
            if (!fn(adaptador, saida)) return;
        }
    }
}

}  // namespace

struct ScreenCapture::Interno {
    ComPtr<ID3D11Device> dispositivo;
    ComPtr<ID3D11DeviceContext> contexto;
    ComPtr<IDXGIOutputDuplication> duplicacao;
    ComPtr<IDXGIAdapter1> adaptador;
    ComPtr<IDXGIOutput1> saida;
    ComPtr<ID3D11Texture2D> texturaAtual;

    MonitorInfo info;
    bool quadroEmMaos = false;

    bool criarDuplicacao();
};

ScreenCapture::ScreenCapture() : d_(std::make_unique<Interno>()) {}
ScreenCapture::~ScreenCapture() { parar(); }

std::vector<MonitorInfo> ScreenCapture::listarMonitores() {
    std::vector<MonitorInfo> monitores;
    uint32_t indice = 0;

    paraCadaSaida([&](ComPtr<IDXGIAdapter1>&, ComPtr<IDXGIOutput>& saida) {
        DXGI_OUTPUT_DESC descricao{};
        if (SUCCEEDED(saida->GetDesc(&descricao)) && descricao.AttachedToDesktop) {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            const bool primario = ::GetMonitorInfoW(descricao.Monitor, &mi) &&
                                  (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

            monitores.push_back(MonitorInfo{
                indice,
                paraUtf8(descricao.DeviceName),
                static_cast<uint32_t>(descricao.DesktopCoordinates.right -
                                      descricao.DesktopCoordinates.left),
                static_cast<uint32_t>(descricao.DesktopCoordinates.bottom -
                                      descricao.DesktopCoordinates.top),
                primario,
            });
            ++indice;
        }
        return true;
    });
    return monitores;
}

bool ScreenCapture::iniciar(uint32_t indiceMonitor) {
    parar();

    uint32_t visto = 0;
    paraCadaSaida([&](ComPtr<IDXGIAdapter1>& adaptador, ComPtr<IDXGIOutput>& saida) {
        DXGI_OUTPUT_DESC descricao{};
        if (FAILED(saida->GetDesc(&descricao)) || !descricao.AttachedToDesktop) return true;
        if (visto++ != indiceMonitor) return true;

        d_->adaptador = adaptador;
        saida.As(&d_->saida);
        d_->info.indice = indiceMonitor;
        d_->info.nome = paraUtf8(descricao.DeviceName);
        d_->info.largura = static_cast<uint32_t>(descricao.DesktopCoordinates.right -
                                                 descricao.DesktopCoordinates.left);
        d_->info.altura = static_cast<uint32_t>(descricao.DesktopCoordinates.bottom -
                                               descricao.DesktopCoordinates.top);
        return false;  // achou, para de procurar
    });

    if (!d_->saida) {
        erro("monitor {} nao encontrado", indiceMonitor);
        return false;
    }

    // O dispositivo D3D11 precisa ser do MESMO adaptador da saída, senão o
    // DuplicateOutput devolve DXGI_ERROR_UNSUPPORTED. É o caso comum em
    // notebook com placa híbrida.
    const D3D_FEATURE_LEVEL niveis[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
    };
    D3D_FEATURE_LEVEL nivelObtido{};

    HRESULT resultado = ::D3D11CreateDevice(
        d_->adaptador.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, niveis, ARRAYSIZE(niveis), D3D11_SDK_VERSION,
        &d_->dispositivo, &nivelObtido, &d_->contexto);
    if (FAILED(resultado)) {
        erro("D3D11CreateDevice falhou: {}", hr(resultado));
        return false;
    }

    if (!d_->criarDuplicacao()) return false;

    info("tela: monitor {} ({}) {}x{}", d_->info.indice, d_->info.nome, d_->info.largura,
         d_->info.altura);
    return true;
}

bool ScreenCapture::Interno::criarDuplicacao() {
    duplicacao.Reset();
    const HRESULT resultado = saida->DuplicateOutput(dispositivo.Get(), &duplicacao);
    if (FAILED(resultado)) {
        if (resultado == E_ACCESSDENIED) {
            // Acontece durante o UAC, na tela de bloqueio e com jogo em tela
            // cheia exclusiva. Não é fatal: tenta de novo depois.
            aviso("duplicacao negada agora (tela protegida ou jogo em tela cheia exclusiva)");
        } else if (resultado == DXGI_ERROR_UNSUPPORTED) {
            erro("duplicacao nao suportada neste adaptador: {}", hr(resultado));
        } else {
            erro("DuplicateOutput falhou: {}", hr(resultado));
        }
        return false;
    }
    return true;
}

bool ScreenCapture::reiniciar() {
    liberarQuadro();
    return d_->criarDuplicacao();
}

void ScreenCapture::parar() {
    liberarQuadro();
    d_->duplicacao.Reset();
    d_->saida.Reset();
    d_->adaptador.Reset();
    d_->contexto.Reset();
    d_->dispositivo.Reset();
}

ResultadoQuadro ScreenCapture::proximoQuadro(uint32_t prazoMs, QuadroCapturado& saida) {
    if (!d_->duplicacao) return ResultadoQuadro::PrecisaReiniciar;
    if (d_->quadroEmMaos) {
        // O DXGI exige ReleaseFrame antes do próximo Acquire. Chamar duas vezes
        // seguidas sem liberar devolve erro e trava a captura.
        liberarQuadro();
    }

    DXGI_OUTDUPL_FRAME_INFO informacao{};
    ComPtr<IDXGIResource> recurso;

    const HRESULT resultado = d_->duplicacao->AcquireNextFrame(prazoMs, &informacao, &recurso);
    if (resultado == DXGI_ERROR_WAIT_TIMEOUT) {
        return ResultadoQuadro::SemMudanca;
    }
    if (resultado == DXGI_ERROR_ACCESS_LOST) {
        return ResultadoQuadro::PrecisaReiniciar;
    }
    if (FAILED(resultado)) {
        erro("AcquireNextFrame falhou: {}", hr(resultado));
        return ResultadoQuadro::Erro;
    }

    d_->quadroEmMaos = true;

    if (informacao.LastPresentTime.QuadPart == 0) {
        // Só o cursor se moveu: o DXGI sinaliza, mas a imagem é a mesma de
        // antes. Codificar isso de novo seria banda jogada fora.
        liberarQuadro();
        return ResultadoQuadro::SemMudanca;
    }

    if (FAILED(recurso.As(&d_->texturaAtual))) {
        liberarQuadro();
        return ResultadoQuadro::Erro;
    }

    saida.textura = d_->texturaAtual.Get();
    saida.largura = d_->info.largura;
    saida.altura = d_->info.altura;
    saida.quadrosAcumulados = informacao.AccumulatedFrames;
    saida.latenciaUs =
        (agoraQpc() - informacao.LastPresentTime.QuadPart) * 1'000'000 / frequenciaQpc();
    saida.cursorVisivel = informacao.PointerPosition.Visible != FALSE;
    saida.cursorX = informacao.PointerPosition.Position.x;
    saida.cursorY = informacao.PointerPosition.Position.y;

    return ResultadoQuadro::Ok;
}

void ScreenCapture::liberarQuadro() {
    if (!d_->quadroEmMaos) return;
    d_->texturaAtual.Reset();
    if (d_->duplicacao) d_->duplicacao->ReleaseFrame();
    d_->quadroEmMaos = false;
}

ID3D11Device* ScreenCapture::dispositivo() const { return d_->dispositivo.Get(); }
ID3D11DeviceContext* ScreenCapture::contexto() const { return d_->contexto.Get(); }
const MonitorInfo& ScreenCapture::monitor() const { return d_->info; }

}  // namespace gl
