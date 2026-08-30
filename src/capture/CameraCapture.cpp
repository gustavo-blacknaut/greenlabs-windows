#include "capture/CameraCapture.h"

#include <windows.h>

#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "util/Log.h"

using Microsoft::WRL::ComPtr;

namespace gl {
namespace {

std::string paraUtf8(const wchar_t* w) {
    if (!w) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string saida(n > 1 ? static_cast<size_t>(n - 1) : 0, 0);
    if (n > 1) ::WideCharToMultiByte(CP_UTF8, 0, w, -1, saida.data(), n, nullptr, nullptr);
    return saida;
}

std::wstring paraW(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring saida(n > 1 ? static_cast<size_t>(n - 1) : 0, 0);
    if (n > 1) ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, saida.data(), n);
    return saida;
}

// Media Foundation precisa ser iniciada uma vez por processo. Fazer isso na
// primeira chamada, e não no arranque, é o que permite listar câmeras sem
// carregar nada em quem nunca abre a lista.
void garantirMediaFoundation() {
    static const bool iniciada = [] {
        const HRESULT r = ::MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        if (FAILED(r)) erro("MFStartup falhou: {}", hr(r));
        return SUCCEEDED(r);
    }();
    (void)iniciada;
}

}  // namespace

struct CameraCapture::Interno {
    ComPtr<ID3D11Device> dispositivo;
    ComPtr<ID3D11DeviceContext> contexto;
    ComPtr<IMFSourceReader> leitor;

    // Duas texturas em rodízio, pelo mesmo motivo do resto do aplicativo:
    // aquela em que a thread da câmera escreve nunca é a que o Video Processor
    // está lendo.
    static constexpr int kBuffers = 2;
    ComPtr<ID3D11Texture2D> destino[kBuffers];
    int proximo = 0;

    // Intermediária que a CPU consegue escrever. A câmera entrega na memória
    // principal de qualquer jeito - não há caminho em que o quadro dela já
    // esteja na GPU - então a subida é inevitável; o que dá para evitar é
    // fazê-la numa textura que o Video Processor recuse, e é por isso que a
    // textura final não tem flag de ligação nenhuma.
    ComPtr<ID3D11Texture2D> intermediaria;

    std::mutex trava;
    ID3D11Texture2D* pronto = nullptr;

    uint32_t largura = 0;
    uint32_t altura = 0;
    std::string nome;

    std::thread thread;
    std::atomic<bool> lendo{false};

    void laco();
    bool publicar(const BYTE* dados, LONG passo, DWORD tamanho);
};

CameraCapture::CameraCapture() : d_(std::make_unique<Interno>()) {}
CameraCapture::~CameraCapture() { parar(); }

std::vector<CameraInfo> CameraCapture::listar() {
    garantirMediaFoundation();

    std::vector<CameraInfo> saida;

    ComPtr<IMFAttributes> atributos;
    if (FAILED(::MFCreateAttributes(&atributos, 1))) return saida;

    // VIDCAP e só VIDCAP. É esta linha que garante que nenhum microfone é
    // tocado: pedir MFMediaSource genérico traria as fontes de áudio junto.
    if (FAILED(atributos->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                  MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
        return saida;
    }

    IMFActivate** dispositivos = nullptr;
    UINT32 total = 0;
    if (FAILED(::MFEnumDeviceSources(atributos.Get(), &dispositivos, &total))) return saida;

    for (UINT32 i = 0; i < total; ++i) {
        CameraInfo info;

        WCHAR* texto = nullptr;
        UINT32 tamanho = 0;
        if (SUCCEEDED(dispositivos[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &texto, &tamanho))) {
            info.id = paraUtf8(texto);
            ::CoTaskMemFree(texto);
        }
        if (SUCCEEDED(dispositivos[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                                          &texto, &tamanho))) {
            info.nome = paraUtf8(texto);
            ::CoTaskMemFree(texto);
        }
        if (info.nome.empty()) info.nome = "Câmera " + std::to_string(i + 1);
        if (!info.id.empty()) saida.push_back(std::move(info));

        dispositivos[i]->Release();
    }
    ::CoTaskMemFree(dispositivos);
    return saida;
}

bool CameraCapture::iniciar(const std::string& id, ID3D11Device* dispositivo,
                            ID3D11DeviceContext* contexto) {
    parar();
    garantirMediaFoundation();

    if (!dispositivo || !contexto) return false;
    d_->dispositivo = dispositivo;
    d_->contexto = contexto;

    const auto cameras = listar();
    if (cameras.empty()) {
        aviso("nenhuma camera encontrada");
        return false;
    }

    // Guardar o caminho e não o índice é o que faz a escolha sobreviver a
    // plugar um segundo dispositivo: com índice, ligar um adaptador de captura
    // trocaria silenciosamente a câmera escolhida pela nova.
    //
    // E pedir uma que não está mais aí é falha, não motivo para pegar outra:
    // ligar a webcam errada por conta própria é o oposto do que se espera de um
    // programa que acende uma luz na cara de alguém.
    const CameraInfo* escolhida = nullptr;
    for (const auto& c : cameras) {
        if (c.id == id) { escolhida = &c; break; }
    }
    if (!escolhida) {
        if (!id.empty()) {
            aviso("a camera escolhida nao esta mais ligada; seguindo sem camera");
            return false;
        }
        escolhida = &cameras.front();
    }

    ComPtr<IMFAttributes> atributos;
    if (FAILED(::MFCreateAttributes(&atributos, 2))) return false;
    atributos->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                       MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    atributos->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                         paraW(escolhida->id).c_str());

    ComPtr<IMFMediaSource> fonte;
    HRESULT resultado = ::MFCreateDeviceSource(atributos.Get(), &fonte);
    if (FAILED(resultado)) {
        // Câmera ocupada por outro programa é o caso mais comum aqui, e não é
        // motivo para o aplicativo parar: dá para transmitir a tela sem ela.
        aviso("nao foi possivel abrir a camera '{}': {}", escolhida->nome, hr(resultado));
        return false;
    }

    ComPtr<IMFAttributes> doLeitor;
    if (FAILED(::MFCreateAttributes(&doLeitor, 1))) return false;
    // Deixa o próprio leitor converter o que a câmera entrega - MJPEG, YUY2, o
    // que for - para NV12. Sem isto seria preciso um decodificador por formato,
    // e cada webcam tem o seu.
    doLeitor->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

    resultado = ::MFCreateSourceReaderFromMediaSource(fonte.Get(), doLeitor.Get(), &d_->leitor);
    if (FAILED(resultado)) {
        erro("MFCreateSourceReaderFromMediaSource falhou: {}", hr(resultado));
        return false;
    }

    ComPtr<IMFMediaType> tipo;
    if (FAILED(::MFCreateMediaType(&tipo))) return false;
    tipo->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    tipo->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);

    resultado = d_->leitor->SetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, tipo.Get());
    if (FAILED(resultado)) {
        erro("a camera nao aceitou NV12: {}", hr(resultado));
        d_->leitor.Reset();
        return false;
    }

    ComPtr<IMFMediaType> efetivo;
    if (FAILED(d_->leitor->GetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &efetivo))) {
        d_->leitor.Reset();
        return false;
    }
    UINT32 larg = 0, alt = 0;
    ::MFGetAttributeSize(efetivo.Get(), MF_MT_FRAME_SIZE, &larg, &alt);
    if (larg == 0 || alt == 0) {
        erro("a camera nao informou o tamanho do quadro");
        d_->leitor.Reset();
        return false;
    }

    // Par, pelo mesmo motivo de sempre: o NV12 subamostra a cor pela metade e
    // dimensão ímpar vira artefato na borda.
    d_->largura = larg & ~1u;
    d_->altura = alt & ~1u;
    d_->nome = escolhida->nome;

    D3D11_TEXTURE2D_DESC descricao{};
    descricao.Width = d_->largura;
    descricao.Height = d_->altura;
    descricao.MipLevels = 1;
    descricao.ArraySize = 1;
    descricao.Format = DXGI_FORMAT_NV12;
    descricao.SampleDesc.Count = 1;
    descricao.Usage = D3D11_USAGE_STAGING;
    descricao.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    descricao.BindFlags = 0;
    if (FAILED(d_->dispositivo->CreateTexture2D(&descricao, nullptr, &d_->intermediaria))) {
        erro("nao foi possivel criar a textura intermediaria da camera");
        d_->leitor.Reset();
        return false;
    }

    // Sem flag de ligação nenhuma: esta textura só é destino de cópia e entrada
    // do Video Processor. SHADER_RESOURCE aqui é o que faz o
    // CreateVideoProcessorInputView recusar a textura com E_INVALIDARG em placa
    // da AMD - foi exatamente esse o motivo de a tela dos outros nunca aparecer.
    descricao.Usage = D3D11_USAGE_DEFAULT;
    descricao.CPUAccessFlags = 0;
    for (int i = 0; i < Interno::kBuffers; ++i) {
        if (FAILED(d_->dispositivo->CreateTexture2D(&descricao, nullptr, &d_->destino[i]))) {
            erro("nao foi possivel criar a textura da camera");
            d_->leitor.Reset();
            return false;
        }
    }

    d_->lendo.store(true);
    d_->thread = std::thread([this] { d_->laco(); });

    info("camera: {} em {}x{}", d_->nome, d_->largura, d_->altura);
    return true;
}

void CameraCapture::parar() {
    d_->lendo.store(false);
    if (d_->thread.joinable()) d_->thread.join();

    {
        std::lock_guard trava(d_->trava);
        d_->pronto = nullptr;
    }
    d_->leitor.Reset();
    d_->intermediaria.Reset();
    for (int i = 0; i < Interno::kBuffers; ++i) d_->destino[i].Reset();
    d_->largura = 0;
    d_->altura = 0;
    d_->nome.clear();
}

bool CameraCapture::ativa() const { return d_->lendo.load() && d_->leitor; }

ID3D11Texture2D* CameraCapture::quadro() {
    std::lock_guard trava(d_->trava);
    return d_->pronto;
}

uint32_t CameraCapture::largura() const { return d_->largura; }
uint32_t CameraCapture::altura() const { return d_->altura; }
const std::string& CameraCapture::nome() const { return d_->nome; }

// Sobe um quadro NV12 da memória principal para a GPU.
//
// O passo que a câmera entrega quase nunca bate com o da textura, então a cópia
// é linha a linha. O plano de cor vem logo depois do de luz, com metade das
// linhas - é o formato NV12 inteiro em duas passadas.
bool CameraCapture::Interno::publicar(const BYTE* dados, LONG passo, DWORD tamanho) {
    if (!dados || passo <= 0) return false;
    const auto necessario = static_cast<DWORD>(passo) * altura * 3 / 2;
    if (tamanho < necessario) return false;

    D3D11_MAPPED_SUBRESOURCE mapeado{};
    if (FAILED(contexto->Map(intermediaria.Get(), 0, D3D11_MAP_WRITE, 0, &mapeado))) return false;

    auto* saida = static_cast<BYTE*>(mapeado.pData);
    const auto largura_ = static_cast<size_t>(largura);

    for (uint32_t linha = 0; linha < altura; ++linha) {
        memcpy(saida + static_cast<size_t>(mapeado.RowPitch) * linha,
               dados + static_cast<size_t>(passo) * linha, largura_);
    }
    const BYTE* corEntrada = dados + static_cast<size_t>(passo) * altura;
    BYTE* corSaida = saida + static_cast<size_t>(mapeado.RowPitch) * altura;
    for (uint32_t linha = 0; linha < altura / 2; ++linha) {
        memcpy(corSaida + static_cast<size_t>(mapeado.RowPitch) * linha,
               corEntrada + static_cast<size_t>(passo) * linha, largura_);
    }

    contexto->Unmap(intermediaria.Get(), 0);

    ID3D11Texture2D* alvo = destino[proximo].Get();
    proximo = (proximo + 1) % kBuffers;
    contexto->CopyResource(alvo, intermediaria.Get());

    std::lock_guard guarda(trava);
    pronto = alvo;
    return true;
}

void CameraCapture::Interno::laco() {
    // A leitura é síncrona e bloqueia até a câmera entregar. Numa thread
    // própria isso não custa nada; no laço da interface custaria o passo da
    // webcam inteiro.
    while (lendo.load()) {
        DWORD fluxo = 0;
        DWORD marcas = 0;
        LONGLONG instante = 0;
        ComPtr<IMFSample> amostra;

        const HRESULT resultado = leitor->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &fluxo, &marcas,
            &instante, &amostra);
        if (FAILED(resultado)) {
            // Camera ja aberta por outro programa entrega o dispositivo e falha na
            // primeira leitura. Sem apagar o "lendo" aqui, ativa() continuava
            // dizendo que sim, a camera entrava na composicao e o quadro dela
            // nunca chegava - com o Blt recebendo superficie nula.
            aviso("leitura da camera falhou: {}", hr(resultado));
            lendo.store(false);
            break;
        }
        if (marcas & MF_SOURCE_READERF_ENDOFSTREAM) {
            lendo.store(false);
            break;
        }
        if (!amostra) continue;  // prazo estourado, sem quadro: normal

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(amostra->ConvertToContiguousBuffer(&buffer))) continue;

        // O buffer de vídeo sabe o passo de verdade; o genérico não, e supor
        // que passo é igual à largura entorta a imagem em toda câmera cujo
        // driver alinha as linhas.
        ComPtr<IMF2DBuffer> doisD;
        if (SUCCEEDED(buffer.As(&doisD))) {
            BYTE* dados = nullptr;
            LONG passo = 0;
            if (SUCCEEDED(doisD->Lock2D(&dados, &passo))) {
                DWORD comprimento = 0;
                doisD->GetContiguousLength(&comprimento);
                publicar(dados, passo, comprimento);
                doisD->Unlock2D();
                continue;
            }
        }

        BYTE* dados = nullptr;
        DWORD comprimento = 0;
        if (SUCCEEDED(buffer->Lock(&dados, nullptr, &comprimento))) {
            publicar(dados, static_cast<LONG>(largura), comprimento);
            buffer->Unlock();
        }
    }
}

}  // namespace gl
