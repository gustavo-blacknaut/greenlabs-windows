# GreenLabs — análise para migração do cliente para C++ nativo

Análise do cliente atual (`C:\Users\geren\Downloads\meet`) feita antes de
escrever qualquer código, conforme pedido. Nenhum arquivo do projeto Electron
foi alterado.

---

## 0. Duas correções de premissa, antes de tudo

**O cliente não é TypeScript.** É JavaScript com JSX, sem um único arquivo
`.ts` ou `.tsx`. Isso importa para a migração: não existe nenhum contrato de
tipos para servir de especificação — o comportamento só está descrito pelo
código em si e pelos comentários. A tradução vai ter que ser lida linha a
linha, não derivada de interfaces.

**O "protocolo de transmissão" atual é WebRTC inteiro.** Isso é o ponto central
desta análise e está detalhado na seção 9. Resumindo aqui porque muda tudo: o
que trafega mídia hoje não é um formato do GreenLabs, é o stack WebRTC do
Chromium — ICE, DTLS, SRTP, negociação SDP e os codecs. O que o servidor de
sinalização carrega é só texto de controle. Tirar o Chromium significa
substituir esse stack por outro compatível com ele, porque do outro lado da
chamada continuam existindo um app Android e um navegador que só falam WebRTC.

---

## 1. Arquitetura atual

### Processos em execução

| Processo | Tecnologia | Papel |
| --- | --- | --- |
| `GreenLabs.exe` (main) | Electron 43 / Node.js | Janela, bandeja, IPC, seletor de fontes, hospedagem, túnel, mute |
| renderer | Chromium | **Toda a mídia**: WebRTC, captura, decodificação, e a UI React |
| `AudioCapture.exe` | C# / .NET Framework 4.0 | WASAPI process loopback, exclusão do Discord |
| `greenlabs-signaling.exe` | Go | Sinalização — só quando o usuário hospeda |
| `cloudflared` / `ngrok` | externo | Túnel público, opcional |
| `powershell.exe` | script | `mute-audio.ps1`, mute por sessão de áudio |

### Tamanho do código

| Arquivo | Linhas | O que é |
| --- | --- | --- |
| `src/main.jsx` | 1719 | Toda a aplicação: WebRTC, estado, UI |
| `src/styles.css` | 2433 | Estilo completo, incluindo responsivo mobile |
| `electron/main.cjs` | 702 | Processo principal |
| `electron/AudioCapture.cs` | 466 | WASAPI process loopback |
| `electron/mute-audio.ps1` | 214 | Mute por sessão |
| `src/icons.jsx` | 186 | Ícones SVG |
| `src/lib/android-screen.js` | 136 | Ponte com o app Android |
| `public/wasapi-audio-worklet.js` | 75 | Ring buffer de áudio |
| `src/lib/wasapi-audio.js` | 71 | Cliente HTTP do áudio |
| `src/lib/media.js` | 67 | Perfis de qualidade, ICE, encoding |
| `electron/preload.cjs` | 41 | Ponte IPC |

Cerca de **5.900 linhas** de lógica própria (fora o CSS). O pacote instalado tem
233 MB, dos quais **225 MB são o Chromium**.

### Entrypoints

```
package.json  "main": "electron/main.cjs"
   │
   ├── app.whenReady()  →  createWindow()  →  carrega index.html
   │        │
   │        └── preload.cjs  →  contextBridge  →  window.greenlabs*
   │
   └── index.html → src/main.jsx → <App/>
```

Em desenvolvimento o renderer vem do Vite (`localhost:5173`); empacotado vem de
`dist/` dentro do asar.

### Comunicação entre processos

Quatro canais distintos, e todos precisam de substituto:

**1. IPC do Electron** (`preload.cjs`, 21 métodos expostos)
Janela, bandeja, autostart, processos em execução, seletor de fontes,
hospedagem, túnel, versão. Vira chamada de função direta em C++ — desaparece.

**2. HTTP local `127.0.0.1:25641`** — áudio
`AudioCapture.exe` serve PCM float32 cru por HTTP com streaming infinito.
Cabeçalhos `X-Sample-Rate` e `X-Channels`. O renderer lê com `fetch` +
`ReadableStream`, desentrelaça os canais e empurra para um `AudioWorklet` com
ring buffer. Em C++ isso vira uma fila em memória no mesmo processo — o cano
HTTP inteiro deixa de existir.

**3. HTTP local `127.0.0.1:8080`** — tela do Android
Mesmo desenho, do lado do app Android. Só existe no mobile; não afeta o
cliente Windows.

**4. WebSocket com o servidor de sinalização** — detalhado na seção 4.

### Captura de tela (hoje)

```
navigator.mediaDevices.getDisplayMedia({ video: {...}, audio: false })
   │
   ├── main.cjs: session.setDisplayMediaRequestHandler
   │      └── desktopCapturer.getSources({ types: ['screen','window'] })
   │             └── manda miniaturas para o renderer, que mostra o seletor
   │
   └── callback({ video: escolhida, audio: 'loopback' })
```

Por baixo, quem captura é o Chromium: DXGI Desktop Duplication para telas
inteiras e Windows Graphics Capture / BitBlt para janelas. **A duplicação DXGI
já é o método em uso** — não é algo a introduzir, é algo a preservar.

Perfis em `src/lib/media.js`: 480p15, 480p30, 720p30, 720p60, 1080p30, 1080p60,
com bitrate de 700 kbps a 7,5 Mbps.

### Captura de áudio (hoje) — **já é nativo**

Esta é a melhor notícia da análise. `AudioCapture.cs` já faz exatamente o que
o pedido descreve no item 3:

- `ActivateAudioInterfaceAsync` no dispositivo virtual `VAD\Process_Loopback`
- `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`
- Modo **EXCLUDE**: captura tudo *menos* a árvore de processos alvo
- Um handler COM cru, porque `ActivateAudioInterfaceAsync` exige um objeto
  agile e o CCW do .NET não serve
- Descoberta do PID do Discord por sessões de áudio ativas + árvore de processos
  via WMI

São 466 linhas de C# que é quase inteiramente interop COM. **Traduzir para C++
deixa esse código mais simples, não mais complicado** — as interfaces
(`IAudioClient`, `IAudioCaptureClient`, `IMMDeviceEnumerator`) são nativas em
C++, então somem as declarações de vtable, os `[Guid]`, o `NativeHandler`
manual e o `Marshal.Copy`.

⚠️ **Cuidado ao portar:** existe uma função `SelfTree()` neste arquivo que na
versão 0.2.7 subia até `explorer.exe` e descia de volta, engolindo o Discord
dentro do "próprio processo" — resultado: o Discord nunca era excluído e o PC
travava. O código atual está correto; ao traduzir, esse comportamento precisa
de teste explícito com o app **iniciado pelo Explorer**, não por terminal (foi
exatamente assim que o defeito passou despercebido).

### Transmissão (hoje)

```
MediaStream (tela)  ──┐
MediaStream (áudio) ──┼──> RTCPeerConnection.addTrack()
                      │         │
                      │         ├── SDP offer/answer (via WebSocket)
                      │         ├── ICE: STUN Google + TURN openrelay.metered.ca
                      │         ├── DTLS handshake
                      │         └── SRTP: pacotes de mídia direto peer-a-peer
                      │
                      └── configureSender(): maxBitrate, maxFramerate,
                          degradationPreference, priority
```

Topologia **mesh**: cada participante abre uma `RTCPeerConnection` com todos os
outros. 30 pessoas = 435 conexões no total, 29 por máquina.

O codec real não está fixado no código — é negociado. **Não confirmei qual
codec está sendo escolhido numa chamada real**, e isso precisa ser medido antes
de qualquer decisão de encoder (ver seção 8, etapa 0).

### Gerenciamento de salas

Sala é só uma string. `createPeer` / `removePeer` mantêm `Map<peerId,
RTCPeerConnection>`, `Map<peerId, nome>` e `Map<"peerId:streamId", metadados>`.
Colisão de oferta é resolvida por *perfect negotiation* — o peer com id
lexicograficamente menor é o "polido" e faz rollback.

### Autenticação

**Não existe.** Nenhum token, senha, login ou sessão em nenhum lugar do cliente,
do servidor ou do site. Quem souber o endereço e o nome da sala entra. A pasta
`auth/` da arquitetura proposta no pedido não tem nada para migrar — seria
funcionalidade nova.

### Configurações

`localStorage` do Chromium, 15 chaves:

```
greenlabs:defaultServer   greenlabs:defaultRoom     greenlabs:servers
greenlabs:userName        greenlabs:hwAccel         greenlabs:shareAudio
greenlabs:audioFilterMode greenlabs:excludedAudioApps
greenlabs:hostPort        greenlabs:hostTunnel      greenlabs:gridSlots
greenlabs:onboarded
```

Em C++ vira um JSON em `%APPDATA%\GreenLabs\config.json`. **A migração precisa
ler o localStorage existente uma vez** (fica em um LevelDB dentro de
`%APPDATA%\GreenLabs\Local Storage\leveldb`) ou aceitar que o usuário reconfigure.
Ler LevelDB só para isso não vale — melhor pedir para reconfigurar uma vez.

### UI

7 componentes React, 2433 linhas de CSS:

| Componente | Função |
| --- | --- |
| `TitleBar` | Barra sem moldura, minimizar/maximizar/fechar, versão |
| `App` | Estado inteiro da aplicação |
| `StreamCard` | Miniatura na lista lateral, volume, ocultar, parar |
| `VideoPlayer` | `<video srcObject>` |
| `ZoomPane` | Zoom e pan com roda do mouse e arrasto |
| `CameraPreview` | Prévia no seletor de câmera |
| `HiddenVisual` | Placeholder de prévia oculta |

Telas: onboarding, modal de configuração com 3 abas (conexão / hospedar /
servidores), seletor de fontes com telas e janelas, palco com grade de 1/2/4,
painel lateral de transmissões, barra inferior mobile, tela cheia.

### Build e distribuição

```
vite build              → dist/  (bundle de 247 KB, 75 KB gzip)
electron-builder --win  → NSIS one-click, 106 MB
csc.exe AudioCapture.cs → AudioCapture.exe (18 KB)
```

Sem assinatura de código válida — o electron-builder usa `signtool` com
certificado de teste.

---

## 2. Arquitetura proposta

```
GreenLabs.exe  (nativo, sem Electron/Chromium/Node)
│
├─ ui/                      Dear ImGui + D3D11
│   └─ apresentação; não conhece rede nem captura
│
├─ src/streaming/           orquestra: quem está na sala, o que está sendo enviado
│   │                       (é a tradução do que hoje vive dentro do App())
│   ├─ StreamManager
│   └─ PeerSession
│
├─ src/capture/
│   ├─ ScreenCapture        DXGI Desktop Duplication → ID3D11Texture2D
│   ├─ WindowCapture        Windows.Graphics.Capture (janelas)
│   └─ AudioCapture         WASAPI process loopback EXCLUDE (porte do .cs)
│
├─ src/encoder/
│   ├─ VideoEncoder         NVENC / AMF / QuickSync via Media Foundation
│   └─ AudioEncoder         Opus (libopus)
│
├─ src/video/
│   └─ Decoder + Renderer   D3D11VA → textura NV12 → shader (zero-copy)
│
├─ src/network/
│   ├─ WebSocketClient      sinalização
│   ├─ Protocol             (de)serialização das mensagens
│   ├─ PeerConnection       ICE/DTLS/SRTP via libdatachannel
│   └─ RtpPacketizer        H.264 (RFC 6184) e Opus (RFC 7587)
│
├─ src/rooms/               estado da sala, participantes, pings
├─ src/config/              JSON em %APPDATA%
└─ src/main/Application     ciclo de vida, janela, bandeja
```

Regras de acoplamento:

- `capture`, `encoder` e `network` **não incluem nada de `ui/`**
- `ui/` fala com `streaming/` por uma fila de eventos, nunca direto com sockets
- o núcleo de mídia compila e roda sem UI (dá para ter um `greenlabs-cli.exe`
  que só transmite — útil para teste e para medir latência sem a janela no meio)

### Threads

| Thread | Responsabilidade |
| --- | --- |
| UI | ImGui, D3D11 present, entrada. **Nunca bloqueia.** |
| Captura de vídeo | `AcquireNextFrame` do DXGI, orientado a evento |
| Captura de áudio | WASAPI com `SetEventHandle`, prioridade realtime (MMCSS) |
| Encoder | fila de 2–3 frames, descarta o mais antigo se atrasar |
| Rede | libdatachannel, uma thread para o loop de eventos |
| Decoder | um por peer, entrega textura direto para a UI |

Comunicação entre threads por fila lock-free de tamanho fixo (SPSC). Nada de
mutex no caminho de frame.

---

## 3. Arquivos que serão removidos

**Nenhum, por enquanto.** O pedido é explícito: o Electron não sai até o C++
funcionar. E há uma razão a mais para não apagar — o cliente web e o app Android
continuam existindo e continuam usando o mesmo servidor.

Quando o C++ estiver equivalente, saem do escopo do cliente Windows:

| Arquivo | Motivo |
| --- | --- |
| `electron/main.cjs` | vira `src/main/Application.cpp` |
| `electron/preload.cjs` | IPC deixa de existir |
| `src/main.jsx` | vira `ui/` + `src/streaming/` |
| `src/styles.css` | ImGui não usa CSS |
| `src/icons.jsx` | ícones viram atlas de textura ou fonte de ícones |
| `src/lib/wasapi-audio.js` | o cano HTTP some, áudio fica em memória |
| `public/wasapi-audio-worklet.js` | ring buffer vira C++ |
| `index.html`, `vite.config.js` | sem bundler |
| `package.json`, `node_modules` | sem npm |

`electron/AudioCapture.cs`, `.exe` e `mute-audio.ps1` **não são removidos** —
viram referência de tradução e ficam no repositório antigo.

`server/` também fica: é usado pelo cliente web.

---

## 4. Arquivos que serão migrados

### 4.1 Protocolo de sinalização — migra 1:1

Este eu conheço com precisão porque reescrevi o servidor em Go. JSON sobre
WebSocket, sem framing binário, sem compressão, sem autenticação.

**Cliente → servidor**

```jsonc
{ "type": "join",  "roomId": "call1", "name": "Fulano" }
{ "type": "ping",  "timestamp": 1700000000000, "rtt": 42 }

// tudo abaixo tem "to" e é repassado ao destinatário com "from" carimbado
{ "type": "offer",        "to": "<peerId>", "description": { "type": "offer",  "sdp": "..." } }
{ "type": "answer",       "to": "<peerId>", "description": { "type": "answer", "sdp": "..." } }
{ "type": "ice",          "to": "<peerId>", "candidate": { "candidate": "...", "sdpMid": "0", "sdpMLineIndex": 0 } }
{ "type": "stream-meta",  "to": "<peerId>", "streamId": "...", "kind": "screen|camera",
                          "name": "Tela 1 - 1080p 30fps", "ownerName": "Fulano",
                          "quality": { "id": "1080p30", "width": 1920, "height": 1080, "fps": 30, "bitrate": 4500000 } }
{ "type": "stream-ended", "to": "<peerId>", "id": "...", "streamId": "..." }
```

**Servidor → cliente**

```jsonc
{ "type": "joined",      "peerId": "<uuid>", "peers": [{ "peerId": "...", "name": "...", "pingMs": 0 }], "count": 2 }
{ "type": "peer-joined", "peerId": "<uuid>", "name": "...", "count": 2 }
{ "type": "peer-left",   "peerId": "<uuid>" }
{ "type": "pong",        "timestamp": <o mesmo enviado>, "serverTime": <ms> }
{ "type": "room-pings",  "pings": { "<peerId>": 42 } }
```

Detalhes que o cliente C++ precisa respeitar:

- **`stream-meta` e `stream-ended` são invenção do GreenLabs**, não do WebRTC. O
  servidor não os interpreta, só repassa. É por eles que o receptor sabe se um
  stream é tela ou câmera, e de quem é.
- **O RTT é medido pelo cliente.** Ele envia `rtt` calculado no relógio dele; o
  servidor só guarda e redistribui. Calcular no servidor mediria descompasso de
  relógio, não latência.
- **Perfect negotiation:** ao receber uma oferta com `signalingState != stable`,
  o peer com `peerId` lexicograficamente **menor** é o polido e faz
  `setRemoteDescription({type:'rollback'})`; o impolido ignora a oferta.
- Ping a cada 1 s; o servidor devolve os pings da sala 1× por segundo.

**Isso não muda.** O servidor em Go e o em Node continuam servindo os três
clientes.

### 4.2 `AudioCapture.cs` → `src/capture/AudioCapture.cpp`

Tradução direta, e mais curta que o original. O que precisa vir junto:

| Do C# | Para o C++ |
| --- | --- |
| `NativeHandler` (vtable COM manual) | classe que implementa `IActivateAudioInterfaceCompletionHandler` e `IAgileObject` — some a gambiarra |
| `PidsWithAudioSessions()` | `IAudioSessionManager2` + `IAudioSessionEnumerator`, igual |
| `ParentMap()` via WMI | `CreateToolhelp32Snapshot` — mais rápido e sem WMI (era 2 consultas WMI por conexão, causa de travamento na 0.2.7) |
| `ReadAvailable()` + `List<float>` | ponteiro direto do `GetBuffer` para a fila, **sem cópia** |
| servidor `HttpListener` | apagado — fila em memória |

O retry de 5 tentativas na porta 25641 também some, já que a porta some.

### 4.3 `wasapi-audio-worklet.js` → ring buffer C++

A lógica de `maxFill` adaptativo (piso de 40 ms, crescendo até 2× a maior
rajada) foi resultado de medição — um teto fixo de 40 ms tocava só 66% do
áudio. **Esse número não é chute e deve ser preservado.**

### 4.4 `src/lib/media.js` → `src/config/Quality.h`

Os 6 perfis e o `configureSender` viram configuração do encoder. Direto.

### 4.5 A lógica de sala dentro de `App()` → `src/rooms/` + `src/streaming/`

O trecho entre as linhas ~508 e ~740 de `src/main.jsx` é o coração: `makeOffer`,
`createPeer`, `removePeer`, `connect`, `addLocalStream`, `removeLocalStream`.
São cerca de 230 linhas que definem todo o comportamento de sala. Devem ser
traduzidas mantendo a ordem das operações — a ordem importa. Dois exemplos
reais de defeitos já corrigidos que a tradução pode reintroduzir:

- `pc.addTrack()` sem renegociar: o áudio é realmente enviado, mas o `ontrack`
  do outro lado nunca dispara e o som nunca chega.
- Colocar o áudio em uma `MediaStream` **nova** em vez de adicionar à existente
  cria um segundo card remoto rotulado "camera".

---

## 5. Arquivos que serão criados

```
greenlabs-windows/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── README.md
├── ANALISE-MIGRACAO.md            ← este documento
│
├── src/
│   ├── main/            Application.{h,cpp}  Window.{h,cpp}  TrayIcon.{h,cpp}
│   ├── capture/         ScreenCapture  WindowCapture  AudioCapture  ProcessTree
│   ├── audio/           RingBuffer  AudioResampler  AudioMixer
│   ├── video/           FrameQueue  VideoDecoder  VideoRenderer
│   ├── encoder/         VideoEncoder  AudioEncoder  EncoderFactory
│   ├── network/         WebSocketClient  Protocol  Messages  PeerConnection  RtpPacketizer
│   ├── streaming/       StreamManager  PeerSession  QualityController
│   ├── rooms/           Room  Participant
│   ├── config/          Config  Quality
│   └── util/            Log  Result  SpscQueue  ComPtr
│
├── ui/                  ImGuiLayer  MainView  StreamPanel  SettingsView
│                        SourcePicker  Onboarding  Theme  Icons
│
├── third_party/         (submódulos ou vcpkg)
├── shaders/             nv12_to_rgb.hlsl
└── tools/               dump-sdp/  (ferramenta da etapa 0)
```

---

## 6. Dependências C++

| Área | Escolha | Por quê |
| --- | --- | --- |
| Build | CMake ≥ 3.25 + vcpkg (manifest) | reprodutível, `vcpkg.json` versionado |
| Compilador | MSVC 19.3x (VS 2022), C++20 | precisa da Windows SDK de qualquer forma |
| Captura de tela | **DXGI Desktop Duplication** (Windows SDK) | sem dependência; já é o que o Chromium usa |
| Captura de janela | **Windows.Graphics.Capture** (WinRT) | ⚠️ desenha borda amarela no Win10; ver riscos |
| Captura de áudio | **WASAPI** (Windows SDK) | sem dependência |
| Encoder de vídeo | **Media Foundation H.264** (NVENC/AMF/QSV por baixo) | uma API para as três GPUs |
| Encoder de áudio | **libopus** (vcpkg) | é o que o WebRTC usa |
| Decoder | **Media Foundation** + D3D11VA | textura direto na GPU, zero-copy |
| ICE/DTLS/SRTP | **libdatachannel** (vcpkg, MPL-2.0) | ~500 KB contra centenas de MB do libwebrtc |
| WebSocket | libdatachannel já traz (`rtc::WebSocket`) | uma dependência a menos |
| JSON | **nlohmann/json** (vcpkg) | header-only |
| UI | **Dear ImGui** + backend D3D11 | vídeo remoto vira textura D3D11 sem sair da GPU |
| Log | **spdlog** (vcpkg) | assíncrono, não bloqueia |

**Descartadas, com motivo:**

- **libwebrtc** — resolveria compatibilidade de uma vez, mas o build precisa de
  `depot_tools`, baixa 2–4 GB de fontes e leva horas. Fica como plano B se a
  interoperabilidade com libdatachannel se mostrar inviável (seção 9).
- **Qt** — resolveria a UI, mas licença comercial ou LGPL com DLLs, e adiciona
  ~40 MB.
- **WebView2** — seria o caminho mais rápido para reaproveitar a UI React
  inteira, mas é o Edge, ou seja **Chromium**. Contraria o requisito.
- **FFmpeg** — pesado para o que é preciso; a Media Foundation cobre o caso.

---

## 7. Estratégia de build

```bash
# uma vez
git clone https://github.com/microsoft/vcpkg third_party/vcpkg
third_party/vcpkg/bootstrap-vcpkg.bat

# configurar
cmake --preset windows-release

# compilar
cmake --build --preset windows-release
```

`vcpkg.json` fixa as versões, então o build é reprodutível. `CMakePresets.json`
com `windows-debug` e `windows-release`.

Release: `/O2 /GL /DNDEBUG`, LTCG, `/MT` para não precisar do redistributable do
Visual C++. Resultado esperado: **um `GreenLabs.exe` de 8–15 MB**, contra 233 MB
hoje. As DLLs de mídia (`mf.dll`, `d3d11.dll`, `dxgi.dll`, `audioclient`) já
vêm com o Windows.

Distribuição:

```
Release/
├── GreenLabs.exe
├── shaders/nv12_to_rgb.cso
└── assets/  (ícones, fonte)
```

O instalador NSIS pode ser reaproveitado quase inteiro.

---

## 8. Estratégia de migração

Por etapas, com o Electron intacto o tempo todo e comparação lado a lado.

### Etapa 0 — medir antes de decidir *(bloqueia tudo)*

Antes de escolher encoder, é preciso saber o que o cliente atual realmente
negocia. Não dá para deduzir do código, porque é negociado em tempo de execução.

1. Abrir uma chamada de verdade entre desktop e Android
2. Capturar o SDP de oferta e resposta (basta logar `pc.localDescription` e
   `pc.remoteDescription` no cliente atual)
3. Registrar: codec de vídeo escolhido, perfil H.264 se for o caso, payload
   types, se há `rtx`, `red`, `ulpfec`, extensões `abs-send-time` e `transport-cc`
4. Fazer o mesmo com o cliente web

**Se o codec negociado for VP8**, o plano de encoder muda: a Media Foundation não
encoda VP8, e aí entra libvpx ou força-se H.264 no SDP (o que exige confirmar
que o Android e o navegador aceitam).

### Etapa 1 — núcleo de captura, sem rede

`ScreenCapture` (DXGI) + `AudioCapture` (WASAPI exclude) + um `greenlabs-probe.exe`
que só mede: frames por segundo, latência de `AcquireNextFrame`, se o Discord
está mesmo fora do áudio. Comparável diretamente com o Electron.

### Etapa 2 — encoder

Media Foundation H.264 com as GPUs disponíveis, medindo latência de encode e
tamanho de GOP. Saída para arquivo, para inspeção.

### Etapa 3 — rede: sinalização

`WebSocketClient` + `Protocol`, entrando numa sala de verdade e aparecendo no
`/rooms` do servidor. Sem mídia ainda. Valida o protocolo inteiro isoladamente.

### Etapa 4 — rede: mídia *(a etapa de risco)*

libdatachannel: ICE, DTLS, SRTP, e envio de RTP H.264. **Critério de sucesso: o
navegador do outro lado exibe o vídeo.** Se isso não funcionar em duas semanas
de trabalho, é o sinal para migrar para libwebrtc.

### Etapa 5 — recepção e renderização

Decodificar os peers com D3D11VA e mostrar. A partir daqui o cliente C++ é
utilizável sem interface.

### Etapa 6 — UI

Só agora, com o núcleo pronto, como o próprio pedido prioriza. Começa pelo
mínimo: conectar, escolher fonte, transmitir, ver. Depois configurações,
onboarding, grade, zoom.

### Etapa 7 — paridade e troca

Rodar os dois em paralelo, comparar latência e uso de recursos, e só então
promover o C++ a cliente padrão.

---

## 9. Problemas e riscos

### 🔴 Risco 1 — Interoperabilidade WebRTC (o que decide o projeto)

Do outro lado da chamada há um **app Android** e um **navegador**, e nenhum dos
dois vai mudar. O cliente C++ precisa conversar com o WebRTC deles.

O libdatachannel implementa ICE, DTLS e SRTP e interopera bem para
DataChannel. Para **mídia** ele é um transporte: não codifica nem empacota, você
entrega pacotes RTP prontos. Isso significa implementar do lado do GreenLabs:

- packetização H.264 em RTP (RFC 6184) com fragmentação FU-A
- packetização Opus (RFC 7587)
- responder a **NACK** (retransmissão) e **PLI/FIR** (pedido de keyframe)
- gerar e ler **RTCP** Sender/Receiver Reports
- **controle de congestionamento** — e aqui está o problema real

O WebRTC do Chromium usa GCC/TWCC para descobrir a banda disponível e ajustar o
bitrate em tempo real. **O libdatachannel não faz isso.** Sem controle de
congestionamento, uma rede que piora não reduz o bitrate: ela perde pacotes, e o
vídeo trava — que é exatamente o sintoma que você já relatou nas versões
anteriores.

Mitigações, em ordem de preferência:
1. Bitrate fixo conservador + reação a perda por RTCP (simples, funciona em rede
   boa, degrada mal em rede ruim)
2. Implementar TWCC + um controlador simples (semanas de trabalho)
3. Trocar para libwebrtc e herdar tudo pronto (dias de build, binário maior,
   mas é o stack que os outros clientes usam)

**Recomendação honesta:** tratar a etapa 4 como um experimento com prazo. Se em
duas semanas o navegador não exibir vídeo do cliente C++ de forma estável, ir
para libwebrtc sem hesitar. O objetivo "sem Chromium" continua atendido — o
libwebrtc é a biblioteca de mídia, não o navegador: nada de Blink, V8, nem
processo de renderização.

### 🟠 Risco 2 — Borda amarela na captura de janela

O pedido menciona explicitamente evitar bordas amarelas. Vale ser preciso sobre
de onde ela vem:

- **DXGI Desktop Duplication** (tela inteira): sem borda. É o caminho principal.
- **Windows.Graphics.Capture** (janela específica): desenha a borda amarela no
  Windows 10. No Windows 11 build 22000+ existe `IsBorderRequired = false`, mas
  **é uma API restrita** — precisa de capacidade declarada em app empacotado.

Se a captura de janela individual for necessária no Windows 10 sem borda, as
opções são `PrintWindow` com `PW_RENDERFULLCONTENT` (mais lento, por CPU) ou
capturar a tela inteira e recortar. Nenhuma é ideal. **Vale confirmar se
capturar janela isolada é mesmo necessário** — hoje o seletor oferece janelas,
mas talvez a maioria dos usuários compartilhe tela inteira.

### 🟠 Risco 3 — A malha de 30 pessoas fica muito mais cara em C++

Hoje o Chromium gerencia 29 `RTCPeerConnection` com codificação compartilhada:
ele codifica **uma vez** e envia para todos. Uma implementação ingênua em C++
com um encoder por peer codificaria 29 vezes o mesmo frame e derrubaria a
máquina.

O desenho precisa ser: **um encoder, N transportes**. O frame codificado é
empacotado uma vez e os mesmos pacotes RTP vão para todos os peers, com
sequência e SSRC por destino. Isso precisa estar na arquitetura desde o começo,
não ser corrigido depois.

### 🟡 Risco 4 — Perda das proteções já conquistadas

Vários números neste código vieram de medição, não de bom senso, e uma tradução
"limpa" tende a normalizá-los de volta ao valor errado:

- `maxFill` adaptativo do ring buffer (fixo em 40 ms tocava 66% do áudio)
- ping acumulado 1×/s por sala (por ping era O(n²): 8 Mbps com 30 pessoas)
- `SelfTree()` — a versão errada engolia o Discord e travava o PC
- renegociação obrigatória depois de `addTrack`

Cada um desses deve virar um **teste**, não um comentário.

### 🟡 Risco 5 — A UI não vai ser igual

São 2433 linhas de CSS com layout responsivo, animação e tema. O ImGui é modo
imediato: dá para chegar perto e funcional, mas **não vai ser visualmente
idêntico**, e tentar reproduzir pixel a pixel consome mais tempo que o resto do
projeto junto. Vale acertar a expectativa agora.

### 🟡 Risco 6 — SmartScreen

Um `.exe` novo e sem assinatura vai levar aviso do Windows nos primeiros
downloads, e o instalador atual já não tem certificado válido. Não é bloqueante,
mas é atrito para quem for instalar.

### 🟢 Risco 7 — Manter dois clientes

Durante a migração, correções de comportamento precisam entrar nos dois. A
mitigação é escopo: congelar funcionalidade nova no Electron durante a migração
e só corrigir defeito.

---

## Resumo executivo

**O que já está pronto:** a captura de áudio com exclusão do Discord — o pedaço
mais difícil e mais específico do projeto — já é código nativo e traduz para C++
ficando mais simples. A captura de tela já usa DXGI. O protocolo de sinalização
está documentado e tem um servidor em Go que eu escrevi e testei.

**O que decide o projeto:** a substituição do WebRTC do Chromium. Não é um
detalhe de implementação, é a espinha dorsal, e o cliente C++ precisa continuar
falando com o Android e com o navegador. A etapa 4 é onde o projeto vive ou
morre, e ela deve ter prazo e critério de sucesso definidos antes de começar.

**O que precisa ser medido antes de qualquer código:** qual codec está
realmente sendo negociado hoje. Isso muda a escolha de encoder e não dá para
deduzir lendo o fonte.

**Ganho esperado:** de 233 MB para 8–15 MB, com latência sob controle direto em
vez de mediada pelo jitter buffer do Chromium.

**Custo realista:** é um projeto de meses, não de semanas — a maior parte
concentrada na etapa 4 e na UI.
