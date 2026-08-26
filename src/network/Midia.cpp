#include "network/Midia.h"

#include <rtc/rtc.hpp>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#include "util/Log.h"

namespace gl {
namespace {

// Payload type que usamos quando somos nós que oferecemos. Quando respondemos,
// o número vem da oferta do outro lado — ver escolherH264().
constexpr int kPayloadH264 = 96;
constexpr uint32_t kRelogioVideo = 90000;  // o relógio de vídeo do RTP é sempre 90 kHz

// Descobre com que número o outro lado chama H.264.
//
// O payload type é combinado por chamada, não fixo pelo padrão. O Chromium
// oferece 96 para VP8 e põe H.264 em outro número. Enviar H.264 marcado como 96
// faz o navegador decodificar como VP8: ele conecta, recebe os pacotes e não
// mostra nada — vídeo 0x0 e nenhum quadro pintado, sem erro em lugar nenhum.
int escolherH264(const rtc::Description::Media& media) {
    int primeiro = -1;
    for (int pt : media.payloadTypes()) {
        const auto* mapa = media.rtpMap(pt);
        if (!mapa || mapa->format != "H264") continue;

        // packetization-mode=1 é o que permite fatiar um quadro grande em
        // vários pacotes (FU-A). Sem ele só cabe quadro que entra num pacote
        // só, o que nunca acontece com tela em 1080p.
        for (const auto& parametro : mapa->fmtps) {
            if (parametro.find("packetization-mode=1") != std::string::npos) return pt;
        }
        if (primeiro < 0) primeiro = pt;
    }
    return primeiro;
}

std::string mensagemDoEstado(rtc::PeerConnection::State estado) {
    using Estado = rtc::PeerConnection::State;
    switch (estado) {
        case Estado::New:          return "novo";
        case Estado::Connecting:   return "conectando";
        case Estado::Connected:    return "conectado";
        case Estado::Disconnected: return "desconectado";
        case Estado::Failed:       return "falhou";
        case Estado::Closed:       return "fechado";
        default:                   return "?";
    }
}

}  // namespace

struct ConexaoPar::Interno {
    std::string par;
    ConfigMidia config;

    std::shared_ptr<rtc::PeerConnection> conexao;
    std::shared_ptr<rtc::Track> faixaVideo;
    std::shared_ptr<rtc::RtpPacketizationConfig> empacotamento;

    AoDescrever aoDescrever;
    AoCandidato aoCandidato;
    AoEstado aoEstado;
    AoPedirChave aoPedirChave;

    std::atomic<bool> aberta{false};
    std::atomic<uint64_t> pacotes{0};
    std::atomic<uint64_t> bytes{0};

    // O primeiro quadro precisa ser chave, e o carimbo de tempo do RTP conta a
    // partir do primeiro envio, não do relógio da máquina.
    int64_t primeiroTempoUs = -1;

    void montarEmpacotador(std::shared_ptr<rtc::Track> faixa);

    std::mutex trava;
    std::string estadoAtual = "novo";
    std::atomic<bool> esperandoResposta{false};
    std::atomic<bool> esperandoPrimeiraChave{true};
};

ConexaoPar::ConexaoPar(std::string idDoPar, const ConfigMidia& config)
    : d_(std::make_unique<Interno>()) {
    d_->par = std::move(idDoPar);
    d_->config = config;

    rtc::Configuration cfg;
    // Os mesmos servidores que o cliente web usa, para os dois acharem o mesmo
    // caminho quando estiverem atrás de roteadores diferentes.
    cfg.iceServers.emplace_back("stun:stun.l.google.com:19302");
    cfg.iceServers.emplace_back("stun:stun1.l.google.com:19302");
    cfg.iceServers.emplace_back(
        rtc::IceServer("openrelay.metered.ca", 443, "openrelayproject", "openrelayproject"));

    // Negociação na mão. Com a automática, adicionar a faixa já disparava uma
    // oferta por conta própria — e como o outro lado também oferece ao entrar,
    // saíam duas negociações para a mesma conexão. O resultado era dois cards
    // do mesmo participante e nenhum quadro chegando.
    cfg.disableAutoNegotiation = true;

    d_->conexao = std::make_shared<rtc::PeerConnection>(cfg);

    d_->conexao->onLocalDescription([this](rtc::Description descricao) {
        if (d_->aoDescrever) {
            d_->aoDescrever(descricao.typeString(), std::string(descricao));
        }
    });

    d_->conexao->onLocalCandidate([this](rtc::Candidate candidato) {
        if (d_->aoCandidato) {
            d_->aoCandidato(std::string(candidato), candidato.mid());
        }
    });

    // Quando somos nós que respondemos, a faixa nasce da oferta do outro lado.
    // Criar uma nossa aqui não adianta: o mid seria outro, ela não casaria com
    // nenhuma m-line da oferta e o vídeo simplesmente não sairia - a conexão
    // ficava "conectada" com zero quadro do outro lado.
    d_->conexao->onTrack([this](std::shared_ptr<rtc::Track> faixa) {
        if (!faixa || faixa->description().type() != "video") return;
        if (d_->faixaVideo) return;
        info("faixa de video recebida da oferta de {} (mid {})", d_->par.substr(0, 8),
             faixa->mid());
        d_->montarEmpacotador(std::move(faixa));
    });

    d_->conexao->onStateChange([this](rtc::PeerConnection::State estado) {
        const bool conectado = estado == rtc::PeerConnection::State::Connected;
        d_->aberta.store(conectado);
        if (conectado) {
            // A conexão acabou de abrir: o que já estava no meio do caminho não
            // serve para começar. Espera o próximo quadro-chave.
            d_->esperandoPrimeiraChave.store(true);
            if (d_->aoPedirChave) d_->aoPedirChave();
        }
        {
            std::lock_guard trava(d_->trava);
            d_->estadoAtual = mensagemDoEstado(estado);
        }
        info("midia com {}: {}", d_->par.substr(0, 8), mensagemDoEstado(estado));
        if (d_->aoEstado) d_->aoEstado(mensagemDoEstado(estado));
    });
}

ConexaoPar::~ConexaoPar() {
    if (d_->conexao) {
        try { d_->conexao->close(); } catch (...) {}
    }
}

void ConexaoPar::aoDescrever(AoDescrever cb) { d_->aoDescrever = std::move(cb); }
void ConexaoPar::aoCandidato(AoCandidato cb) { d_->aoCandidato = std::move(cb); }
void ConexaoPar::aoEstado(AoEstado cb) { d_->aoEstado = std::move(cb); }
void ConexaoPar::aoPedirChave(AoPedirChave cb) { d_->aoPedirChave = std::move(cb); }

bool ConexaoPar::pronto() const { return d_->aberta.load(); }
bool ConexaoPar::ofertaPendente() const { return d_->esperandoResposta.load(); }
const std::string& ConexaoPar::idDoPar() const { return d_->par; }
uint64_t ConexaoPar::pacotesEnviados() const { return d_->pacotes.load(); }
uint64_t ConexaoPar::bytesEnviados() const { return d_->bytes.load(); }

std::string ConexaoPar::estado() const {
    std::lock_guard trava(d_->trava);
    return d_->estadoAtual;
}

void ConexaoPar::Interno::montarEmpacotador(std::shared_ptr<rtc::Track> faixa) {
    const rtc::SSRC ssrc = 42;

    // Quando a faixa veio de uma oferta do outro lado, o número do H.264 é o
    // dele. Quando fomos nós que oferecemos, é o nosso.
    int payload = kPayloadH264;
    if (const int doOutroLado = escolherH264(faixa->description()); doOutroLado >= 0) {
        payload = doOutroLado;
    } else {
        erro("o outro lado nao ofereceu H.264; o video nao vai aparecer para ele");
    }
    info("video com {} usando payload type {}", par.substr(0, 8), payload);

    // A faixa que veio de uma oferta remota não declara SSRC nenhum: ela nasceu
    // recvonly do outro lado. Sem um `a=ssrc:` na nossa resposta, o navegador
    // recebe os pacotes e não sabe para qual receptor entregar - eles chegam no
    // transporte (dá para ver os bytes subindo nas estatísticas) e são
    // descartados antes de virar vídeo. Declarar o SSRC é o que amarra os dois.
    try {
        auto descricao = faixa->description();
        descricao.addSSRC(ssrc, "greenlabs-tela");
        faixa->setDescription(std::move(descricao));
    } catch (const std::exception& e) {
        erro("nao foi possivel declarar o SSRC: {}", e.what());
    }

    empacotamento = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "greenlabs-tela",
                                                                 payload, kRelogioVideo);

    // LongStartSequence: o encoder do Media Foundation entrega Annex-B
    // (00 00 00 01 antes de cada unidade NAL), e é assim que o empacotador sabe
    // onde uma termina e a outra começa.
    auto empacotador = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::StartSequence, empacotamento);

    // Relatórios RTCP: é por eles que o outro lado avisa que perdeu pacote e
    // pede quadro-chave. Sem isso a imagem fica quebrada até o próximo IDR.
    empacotador->addToChain(std::make_shared<rtc::RtcpSrReporter>(empacotamento));
    empacotador->addToChain(std::make_shared<rtc::RtcpNackResponder>());

    // PLI é como o outro lado diz "estou perdido, me manda um quadro-chave".
    // Ignorar isso é o que deixava a tela preta para sempre: ele pedia, nós não
    // respondíamos, e sem IDR o decodificador dele nunca começava.
    empacotador->addToChain(std::make_shared<rtc::PliHandler>([this] {
        esperandoPrimeiraChave.store(true);
        if (aoPedirChave) aoPedirChave();
    }));

    faixa->setMediaHandler(empacotador);
    faixaVideo = std::move(faixa);
}

bool ConexaoPar::prepararFaixa() {
    if (d_->faixaVideo) return true;
    try {
        rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
        video.addH264Codec(kPayloadH264);
        video.addSSRC(42, "greenlabs-tela");
        d_->montarEmpacotador(d_->conexao->addTrack(video));
        return true;
    } catch (const std::exception& e) {
        erro("nao foi possivel preparar a faixa para {}: {}", d_->par.substr(0, 8), e.what());
        return false;
    }
}

bool ConexaoPar::oferecer() {
    if (!prepararFaixa()) return false;
    try {
        d_->conexao->setLocalDescription(rtc::Description::Type::Offer);
        d_->esperandoResposta.store(true);
        info("oferta enviada para {}", d_->par.substr(0, 8));
        return true;
    } catch (const std::exception& e) {
        erro("nao foi possivel oferecer para {}: {}", d_->par.substr(0, 8), e.what());
        return false;
    }
}

void ConexaoPar::receberDescricao(const std::string& tipo, const std::string& sdp) {
    try {
        d_->conexao->setRemoteDescription(rtc::Description(sdp, tipo));
        if (tipo == "answer") {
            d_->esperandoResposta.store(false);
        } else {
            // Com a negociação na mão, a resposta não sai sozinha: é aqui que
            // ela é gerada, e o onLocalDescription a manda pela sinalização.
            d_->conexao->setLocalDescription(rtc::Description::Type::Answer);
        }
        info("{} de {} aceita", tipo, d_->par.substr(0, 8));
    } catch (const std::exception& e) {
        erro("{} de {} recusada: {}", tipo, d_->par.substr(0, 8), e.what());
    }
}

void ConexaoPar::receberCandidato(const std::string& candidato, const std::string& mid) {
    try {
        d_->conexao->addRemoteCandidate(rtc::Candidate(candidato, mid));
    } catch (const std::exception&) {
        // Candidato inválido acontece o tempo todo (rede que sumiu, formato de
        // outro navegador). Não é motivo para derrubar a chamada.
    }
}

void ConexaoPar::enviarVideo(const uint8_t* anexoB, size_t tamanho, int64_t tempoUs,
                             bool chave) {
    if (!d_->faixaVideo || !d_->aberta.load() || tamanho == 0) return;

    // Sem quadro-chave o decodificador do outro lado não tem por onde começar:
    // ele fica recebendo fatia P que depende de um quadro que nunca chegou, e o
    // vídeo fica 0x0 para sempre. Descarta até vir um.
    if (d_->esperandoPrimeiraChave.load()) {
        if (!chave) return;
        d_->esperandoPrimeiraChave.store(false);
        info("primeiro quadro-chave enviado para {}", d_->par.substr(0, 8));
    }

    if (d_->primeiroTempoUs < 0) {
        d_->primeiroTempoUs = tempoUs;
        // Uma vez so: confirma o formato do que sai do encoder em vez de supor.
        std::string inicio;
        for (size_t i = 0; i < 12 && i < tamanho; ++i) {
            char b[4];
            std::snprintf(b, sizeof(b), "%02X ", anexoB[i]);
            inicio += b;
        }
        info("primeiro quadro para {}: {} bytes, comeca com {}", d_->par.substr(0, 8), tamanho,
             inicio);
    }
    const double segundos = static_cast<double>(tempoUs - d_->primeiroTempoUs) / 1'000'000.0;

    try {
        d_->empacotamento->timestamp =
            d_->empacotamento->startTimestamp +
            static_cast<uint32_t>(segundos * static_cast<double>(kRelogioVideo));

        d_->faixaVideo->send(reinterpret_cast<const std::byte*>(anexoB), tamanho);
        d_->pacotes.fetch_add(1, std::memory_order_relaxed);
        d_->bytes.fetch_add(tamanho, std::memory_order_relaxed);
    } catch (const std::exception&) {
        // Fila cheia ou conexão caindo. Descartar o quadro é a resposta certa
        // ao vivo: guardar só empilharia atraso.
    }
}

}  // namespace gl
