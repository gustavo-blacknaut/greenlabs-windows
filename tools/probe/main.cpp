// greenlabs-probe: mede o núcleo de captura sem UI e sem rede.
//
// Existe para comparar com o cliente em Electron em números, não em impressão:
// quantos quadros por segundo a duplicação entrega, quanta latência ela
// acrescenta, e se o áudio do aplicativo excluído está mesmo fora da captura.
//
//   greenlabs-probe --listar
//   greenlabs-probe --segundos 10 --monitor 0 --excluir discord

#include <windows.h>

#include <objbase.h>  // CoInitializeEx: WIN32_LEAN_AND_MEAN tira o COM do windows.h

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <string>
#include <vector>

#include "audio/RingBuffer.h"
#include "capture/AudioCapture.h"
#include "capture/ProcessTree.h"
#include "capture/ScreenCapture.h"
#include "encoder/VideoEncoder.h"
#include "util/Log.h"
#include "video/ColorConverter.h"

namespace {

struct Opcoes {
    bool listar = false;
    uint32_t monitor = 0;
    uint32_t segundos = 10;
    std::vector<std::string> excluir = {"discord", "discordptb", "discordcanary",
                                        "discorddevelopment"};
    bool semAudio = false;
    bool semVideo = false;
    std::string gravar;   // caminho do .h264 a escrever
    uint32_t bitrate = 4500;  // kbps
};

std::vector<std::string> separarPorVirgula(const std::string& texto) {
    std::vector<std::string> partes;
    size_t inicio = 0;
    while (inicio <= texto.size()) {
        const size_t fim = texto.find(',', inicio);
        std::string parte =
            texto.substr(inicio, fim == std::string::npos ? std::string::npos : fim - inicio);
        while (!parte.empty() && parte.front() == ' ') parte.erase(parte.begin());
        while (!parte.empty() && parte.back() == ' ') parte.pop_back();
        if (!parte.empty()) partes.push_back(parte);
        if (fim == std::string::npos) break;
        inicio = fim + 1;
    }
    return partes;
}

Opcoes lerOpcoes(int argc, char** argv) {
    Opcoes o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool temProximo = i + 1 < argc;
        if (arg == "--listar") {
            o.listar = true;
        } else if (arg == "--sem-audio") {
            o.semAudio = true;
        } else if (arg == "--sem-video") {
            o.semVideo = true;
        } else if (arg == "--monitor" && temProximo) {
            o.monitor = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--segundos" && temProximo) {
            o.segundos = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--excluir" && temProximo) {
            o.excluir = separarPorVirgula(argv[++i]);
        } else if (arg == "--gravar" && temProximo) {
            o.gravar = argv[++i];
        } else if (arg == "--bitrate" && temProximo) {
            o.bitrate = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "-h" || arg == "--help") {
            std::puts(
                "greenlabs-probe\n"
                "  --listar              lista os monitores e sai\n"
                "  --monitor N           qual monitor capturar (padrao: 0)\n"
                "  --segundos N          duracao da medicao (padrao: 10)\n"
                "  --excluir a,b,c       nomes a deixar fora do audio\n"
                "                        (padrao: discord e variantes)\n"
                "  --sem-audio           mede so o video\n"
                "  --sem-video           mede so o audio\n"
                "  --gravar saida.h264   codifica em H.264 e grava o fluxo\n"
                "  --bitrate N           kbps do encoder (padrao: 4500)");
            std::exit(0);
        }
    }
    return o;
}

int64_t percentil(std::vector<int64_t>& valores, double p) {
    if (valores.empty()) return 0;
    const auto posicao = static_cast<size_t>(p * static_cast<double>(valores.size() - 1));
    std::nth_element(valores.begin(), valores.begin() + static_cast<ptrdiff_t>(posicao),
                     valores.end());
    return valores[posicao];
}

// compare_exchange em laço: vários pacotes de áudio podem chegar juntos.
template <class T>
void guardarMaior(std::atomic<T>& alvo, T candidato) {
    T atual = alvo.load(std::memory_order_relaxed);
    while (candidato > atual &&
           !alvo.compare_exchange_weak(atual, candidato, std::memory_order_relaxed)) {
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Sem isto o Windows reporta dimensões escaladas em telas com DPI alto e a
    // medição sai errada.
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HRESULT inicioCom = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(inicioCom)) {
        gl::erro("CoInitializeEx falhou: {}", gl::hr(inicioCom));
        return 1;
    }

    const Opcoes opcoes = lerOpcoes(argc, argv);

    const auto monitores = gl::ScreenCapture::listarMonitores();
    if (opcoes.listar || monitores.empty()) {
        std::puts("\nMonitores:");
        for (const auto& m : monitores) {
            std::printf("  [%u] %-16s %ux%u%s\n", m.indice, m.nome.c_str(), m.largura, m.altura,
                        m.primario ? "  (principal)" : "");
        }
        if (monitores.empty()) std::puts("  nenhum monitor encontrado");
        std::puts("");
        ::CoUninitialize();
        return 0;
    }

    // ----------------------------------------------------------------- audio
    gl::AudioCapture audio;
    gl::RingBuffer anel(48000, 2);
    std::atomic<uint64_t> quadrosAudio{0};
    std::atomic<uint64_t> pacotesAudio{0};
    std::atomic<uint32_t> maiorPacote{0};
    std::atomic<double> picoAbsoluto{0.0};

    uint32_t pidExcluido = 0;
    if (!opcoes.semAudio) {
        pidExcluido = gl::acharRaizParaExcluir(opcoes.excluir);
        if (pidExcluido == 0) {
            gl::aviso(
                "nenhum dos aplicativos a excluir esta rodando; a captura vai pegar tudo. "
                "Para testar a exclusao de verdade, abra o Discord e rode de novo.");
        }

        const bool ok = audio.iniciar(pidExcluido, [&](const float* dados, uint32_t quadros) {
            quadrosAudio.fetch_add(quadros, std::memory_order_relaxed);
            pacotesAudio.fetch_add(1, std::memory_order_relaxed);
            guardarMaior(maiorPacote, quadros);

            double pico = 0.0;
            const size_t amostras = static_cast<size_t>(quadros) * 2;
            for (size_t i = 0; i < amostras; ++i) {
                pico = std::max(pico, std::fabs(static_cast<double>(dados[i])));
            }
            guardarMaior(picoAbsoluto, pico);

            anel.escrever(dados, quadros);
        });
        if (!ok) gl::erro("captura de audio nao iniciou; seguindo so com video");
    }

    // Consumidor do anel no ritmo real de reproducao, 10 ms por vez. Sem ele o
    // anel so enche e descarta, e a conta de descartes nao mede nada.
    std::atomic<bool> consumindo{true};
    std::atomic<uint64_t> amostrasReais{0};
    std::atomic<uint64_t> amostrasSilencio{0};
    std::thread consumidor([&] {
        constexpr size_t kAmostrasPorVez = 480 * 2;  // 10 ms em estereo
        std::vector<float> bloco(kAmostrasPorVez);
        auto proximo = std::chrono::steady_clock::now();
        while (consumindo.load(std::memory_order_relaxed)) {
            proximo += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(proximo);
            const size_t reais = anel.ler(bloco.data(), kAmostrasPorVez);
            amostrasReais.fetch_add(reais, std::memory_order_relaxed);
            amostrasSilencio.fetch_add(kAmostrasPorVez - reais, std::memory_order_relaxed);
        }
    });

    // ----------------------------------------------------------------- video
    gl::ScreenCapture tela;
    std::vector<int64_t> latencias;
    uint64_t quadrosVideo = 0;
    uint64_t semMudanca = 0;
    uint64_t reinicios = 0;
    uint64_t acumuladosTotal = 0;
    bool erroFatal = false;

    if (!opcoes.semVideo) {
        if (!tela.iniciar(opcoes.monitor)) {
            gl::erro("captura de tela nao iniciou");
            audio.parar();
            ::CoUninitialize();
            return 1;
        }
        latencias.reserve(static_cast<size_t>(opcoes.segundos) * 200);
    }

    // --------------------------------------------------------------- encoder
    gl::ColorConverter conversor;
    gl::VideoEncoder encoder;
    FILE* arquivo = nullptr;
    std::vector<int64_t> latenciasEncode;
    uint64_t quadrosChave = 0;
    bool codificando = false;

    if (!opcoes.gravar.empty() && !opcoes.semVideo) {
        const auto& m = tela.monitor();
        if (!conversor.iniciar(tela.dispositivo(), tela.contexto(), m.largura, m.altura,
                               m.largura, m.altura)) {
            gl::erro("conversor de cor nao iniciou");
        } else {
            gl::ConfigEncoder cfg;
            cfg.largura = conversor.largura();
            cfg.altura = conversor.altura();
            cfg.fps = 60;
            cfg.bitrate = opcoes.bitrate * 1000;

            arquivo = std::fopen(opcoes.gravar.c_str(), "wb");
            if (!arquivo) {
                gl::erro("nao foi possivel criar {}", opcoes.gravar);
            } else if (encoder.iniciar(tela.dispositivo(), cfg,
                                       [&](const gl::PacoteCodificado& pacote) {
                                           std::fwrite(pacote.dados, 1, pacote.tamanho, arquivo);
                                           if (pacote.chave) ++quadrosChave;
                                       })) {
                codificando = true;
                encoder.pedirQuadroChave();
            }
        }
    }

    gl::info("medindo por {} segundos...", opcoes.segundos);
    const auto inicio = std::chrono::steady_clock::now();
    const auto fim = inicio + std::chrono::seconds(opcoes.segundos);

    while (!erroFatal && std::chrono::steady_clock::now() < fim) {
        if (opcoes.semVideo) {
            ::Sleep(100);
            continue;
        }

        gl::QuadroCapturado quadro;
        switch (tela.proximoQuadro(100, quadro)) {
            case gl::ResultadoQuadro::Ok:
                ++quadrosVideo;
                acumuladosTotal += quadro.quadrosAcumulados;
                latencias.push_back(quadro.latenciaUs);

                if (codificando) {
                    const auto antes = std::chrono::steady_clock::now();
                    // A textura nunca sai da GPU: duplicação -> conversão de cor
                    // -> encoder, tudo no mesmo dispositivo D3D11.
                    if (auto* nv12 = conversor.converter(quadro.textura)) {
                        encoder.codificar(nv12, std::chrono::duration_cast<std::chrono::microseconds>(
                                                    antes - inicio).count());
                    }
                    latenciasEncode.push_back(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - antes).count());
                }

                tela.liberarQuadro();
                break;
            case gl::ResultadoQuadro::SemMudanca:
                ++semMudanca;
                break;
            case gl::ResultadoQuadro::PrecisaReiniciar:
                ++reinicios;
                if (!tela.reiniciar()) ::Sleep(200);
                break;
            case gl::ResultadoQuadro::Erro:
                gl::erro("erro na captura de tela; encerrando");
                erroFatal = true;
                break;
        }
    }

    const double decorrido =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - inicio).count();

    audio.parar();
    if (codificando) encoder.parar();
    if (arquivo) std::fclose(arquivo);
    consumindo.store(false);
    if (consumidor.joinable()) consumidor.join();

    // -------------------------------------------------------------- relatorio
    std::puts("");
    std::puts("========================= RESULTADO =========================");
    std::printf("duracao: %.1fs\n", decorrido);

    if (!opcoes.semVideo) {
        const auto& m = tela.monitor();
        std::puts("");
        std::printf("VIDEO  (DXGI Desktop Duplication, monitor %u %ux%u)\n", m.indice, m.largura,
                    m.altura);
        std::printf("  quadros entregues  : %llu  (%.1f/s)\n", (unsigned long long)quadrosVideo,
                    quadrosVideo / decorrido);
        std::printf("  sem mudanca        : %llu  (tela parada, nao e perda)\n",
                    (unsigned long long)semMudanca);
        std::printf("  quadros coalescidos: %llu  (tela mudou mais rapido que a leitura)\n",
                    (unsigned long long)(acumuladosTotal > quadrosVideo
                                             ? acumuladosTotal - quadrosVideo
                                             : 0));
        std::printf("  reinicios          : %llu\n", (unsigned long long)reinicios);
        if (!latencias.empty()) {
            int64_t soma = 0;
            for (int64_t v : latencias) soma += v;
            std::printf(
                "  latencia present->captura: media %.2f ms | p50 %.2f ms | p95 %.2f ms | max "
                "%.2f ms\n",
                soma / static_cast<double>(latencias.size()) / 1000.0,
                percentil(latencias, 0.50) / 1000.0, percentil(latencias, 0.95) / 1000.0,
                percentil(latencias, 1.00) / 1000.0);
        }
        if (codificando) {
            const double segundos = decorrido > 0 ? decorrido : 1.0;
            std::puts("");
            std::printf("  encoder            : %s (%s)\n", encoder.nomeDoEncoder(),
                        encoder.porHardware() ? "hardware" : "software");
            std::printf("  quadros codificados: %llu  (%llu chave)\n",
                        (unsigned long long)encoder.quadrosCodificados(),
                        (unsigned long long)quadrosChave);
            std::printf("  bitrate real       : %.0f kbps  (alvo %u kbps)\n",
                        encoder.bytesGerados() * 8.0 / segundos / 1000.0, opcoes.bitrate);
            std::printf("  arquivo            : %s  (%.1f MB)\n", opcoes.gravar.c_str(),
                        encoder.bytesGerados() / 1024.0 / 1024.0);
            if (!latenciasEncode.empty()) {
                int64_t soma = 0;
                for (int64_t v : latenciasEncode) soma += v;
                std::printf("  latencia de encode : media %.2f ms | p95 %.2f ms | max %.2f ms\n",
                            soma / static_cast<double>(latenciasEncode.size()) / 1000.0,
                            percentil(latenciasEncode, 0.95) / 1000.0,
                            percentil(latenciasEncode, 1.00) / 1000.0);
            }
            std::puts("");
        }
        std::puts("  borda amarela      : nao (a duplicacao nao desenha nada na tela)");
        std::puts("  cursor             : nao vem no quadro (composicao ainda nao implementada)");
    }

    if (!opcoes.semAudio) {
        const uint64_t quadros = quadrosAudio.load();
        std::puts("");
        std::puts("AUDIO  (WASAPI process loopback, modo EXCLUDE)");
        if (pidExcluido != 0) {
            std::printf("  excluido           : arvore do pid %u (%s)\n", pidExcluido,
                        gl::nomeDoProcesso(pidExcluido).c_str());
        } else {
            std::puts("  excluido           : nada (nenhum dos aplicativos estava rodando)");
        }
        std::printf("  quadros capturados : %llu  (%.0f/s, esperado ~48000/s)\n",
                    (unsigned long long)quadros, quadros / decorrido);
        std::printf("  pacotes            : %llu  (maior: %u quadros = %.1f ms)\n",
                    (unsigned long long)pacotesAudio.load(), maiorPacote.load(),
                    maiorPacote.load() / 48.0);
        std::printf("  pico de amplitude  : %.4f  %s\n", picoAbsoluto.load(),
                    picoAbsoluto.load() > 0.0001
                        ? "(havia som tocando)"
                        : "(silencio: nada tocava, ou tudo estava excluido)");
        std::printf("  descontinuidades   : %llu  (deve ser 0)\n",
                    (unsigned long long)audio.quadrosComFalha());
        const uint64_t reais = amostrasReais.load();
        const uint64_t mudas = amostrasSilencio.load();
        const double aproveitado = (reais + mudas) ? 100.0 * static_cast<double>(reais) /
                                                         static_cast<double>(reais + mudas)
                                                   : 0.0;
        std::printf("  anel: teto %zu amostras (%.1f ms) | maior rajada %zu | descartadas %llu\n",
                    anel.teto(), static_cast<double>(anel.teto()) / 2.0 / 48.0, anel.maiorRajada(),
                    (unsigned long long)anel.descartadas());
        std::printf("  reproducao: %.1f%% de audio real, %.1f%% de silencio de preenchimento\n",
                    aproveitado, 100.0 - aproveitado);
        std::puts("              (no cliente em Electron isto ja mediu 66% antes do teto");
        std::puts("               adaptativo do anel; abaixo de ~98% e sinal de regressao)");
    }

    std::puts("============================================================");
    std::puts("");
    if (pidExcluido != 0) {
        std::puts("Para confirmar a exclusao: toque som no Discord E em outro aplicativo ao");
        std::puts("mesmo tempo. O pico deve subir com o outro aplicativo e NAO subir quando so");
        std::puts("o Discord estiver tocando.");
        std::puts("");
    }

    ::CoUninitialize();
    return erroFatal ? 1 : 0;
}
