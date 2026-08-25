#include "network/Midia.h"

#include <rtc/rtc.hpp>

#include <atomic>
#include <mutex>

#include "util/Log.h"

namespace gl {
namespace {

// Números do RTP que os dois lados precisam combinar. 96 é o primeiro payload
// type dinâmico, e é o que o navegador oferece para H.264.
constexpr int kPayloadH264 = 96;
constexpr uint32_t kRelogioVideo = 90000;  // o relógio de vídeo do RTP é sempre 90 kHz

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

    std::atomic<bool> aberta{false};
    std::atomic<uint64_t> pacotes{0};
    std::atomic<uint64_t> bytes{0};

    // O primeiro quadro precisa ser chave, e o carimbo de tempo do RTP conta a
    // partir do primeiro envio, não do relógio da máquina.
    int64_t primeiroTempoUs = -1;
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

    // Candidatos saem um a um em vez de esperar a coleta terminar. Numa rede
    // boa isso conecta antes de o STUN sequer responder.
    cfg.disableAutoNegotiation = false;

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

    d_->conexao->onStateChange([this](rtc::PeerConnection::State estado) {
        const bool conectado = estado == rtc::PeerConnection::State::Connected;
        d_->aberta.store(conectado);
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

bool ConexaoPar::pronto() const { return d_->aberta.load(); }
const std::string& ConexaoPar::idDoPar() const { return d_->par; }
uint64_t ConexaoPar::pacotesEnviados() const { return d_->pacotes.load(); }
uint64_t ConexaoPar::bytesEnviados() const { return d_->bytes.load(); }

bool ConexaoPar::oferecer() {
    try {
        const rtc::SSRC ssrc = 42;
        rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
        video.addH264Codec(kPayloadH264);
        video.addSSRC(ssrc, "greenlabs-tela");

        d_->faixaVideo = d_->conexao->addTrack(video);

        d_->empacotamento = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc, "greenlabs-tela", kPayloadH264, kRelogioVideo);

        // Separator::StartSequence: o encoder do Media Foundation entrega Annex-B
        // (00 00 00 01 antes de cada unidade NAL), e é assim que o empacotador
        // sabe onde uma termina e a outra começa.
        auto empacotador = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::LongStartSequence, d_->empacotamento);

        // Relatórios RTCP: é por eles que o outro lado avisa que perdeu pacote e
        // pede quadro-chave. Sem isso a imagem fica quebrada até o próximo IDR.
        auto relatorios = std::make_shared<rtc::RtcpSrReporter>(d_->empacotamento);
        empacotador->addToChain(relatorios);
        empacotador->addToChain(std::make_shared<rtc::RtcpNackResponder>());

        d_->faixaVideo->setMediaHandler(empacotador);
        d_->conexao->setLocalDescription();
        return true;
    } catch (const std::exception& e) {
        erro("nao foi possivel oferecer midia para {}: {}", d_->par.substr(0, 8), e.what());
        return false;
    }
}

void ConexaoPar::receberDescricao(const std::string& tipo, const std::string& sdp) {
    try {
        d_->conexao->setRemoteDescription(rtc::Description(sdp, tipo));
    } catch (const std::exception& e) {
        erro("descricao de {} recusada: {}", d_->par.substr(0, 8), e.what());
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

void ConexaoPar::enviarVideo(const uint8_t* anexoB, size_t tamanho, int64_t tempoUs) {
    if (!d_->faixaVideo || !d_->aberta.load() || tamanho == 0) return;

    if (d_->primeiroTempoUs < 0) d_->primeiroTempoUs = tempoUs;
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
