<div align="center">

<img src="assets/logo.png" width="96" alt="GreenLabs">

# GreenLabs para Windows

**Mostre sua tela com o som do jogo — e sem o Discord junto.**

Um executável de 3,8 MB. Não instala nada, não pede conta, não tem limite de tempo.

[![Baixar](https://img.shields.io/badge/Baixar-.exe%20para%20Windows-16A34A?style=for-the-badge)](https://github.com/gustavo-blacknaut/greenlabs-windows/releases/latest)
&nbsp;
![Windows 10+](https://img.shields.io/badge/Windows-10%20ou%2011-0078D4?style=flat-square)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square)
![Sem Electron](https://img.shields.io/badge/sem-Electron-6B7280?style=flat-square)

</div>

---

## O que ele faz

**Transmite o som do sistema menos um aplicativo.** O jogo, o Spotify e o
navegador vão junto com a imagem; o Discord fica de fora — sem ser silenciado
para você. Quem está na chamada ouve o jogo, não a conversa.

**Mostra a tela de todo mundo.** Cada pessoa transmitindo ganha um cartão com
miniatura ao vivo no painel; um clique põe a dela no palco. Duplo clique abre em
tela cheia.

**Coloca você no canto.** A câmera entra composta no próprio quadro, na GPU, sem
custar nada e sem precisar que ninguém atualize nada — quem assiste pelo
navegador ou pelo celular já vê.

**Não atrapalha a internet.** Os pacotes saem espalhados no tempo em vez de em
rajada, que é o que fazia o ping de todo mundo na casa subir durante a
transmissão.

E é leve porque não tem navegador dentro: a janela é Direct2D, a captura é
DXGI, o codec é Media Foundation e o transporte é libdatachannel. Nenhum
Chromium, nenhum Node.

---

## Como usar

1. Baixe o `.zip` na [página de versões](https://github.com/gustavo-blacknaut/greenlabs-windows/releases/latest).
2. Descompacte e abra o `GreenLabs.exe`.
3. Escreva seu apelido, o endereço do servidor e o nome da sala.
4. **ENTRAR NA SALA** e depois **TRANSMITIR**.

No painel da direita ficam monitor, qualidade, câmera, som do sistema e o volume
de quem você ouve. Tudo é lembrado para a próxima vez.

| Atalho | O que faz |
| --- | --- |
| Duplo clique no palco | Entra e sai da tela cheia |
| `F11` | O mesmo |
| `Esc` | Sai da tela cheia |
| Roda do mouse no painel | Rola a lista |

Não tem servidor? O [GreenLabs Server](https://github.com/gustavo-blacknaut/greenlabs-server)
sobe com um comando, e há também o [cliente em Electron](https://github.com/gustavo-blacknaut/greenlabs-desktop),
o [aplicativo Android](https://github.com/gustavo-blacknaut/greenlabs-android) e
o [site](https://github.com/gustavo-blacknaut/greenlabs-site).

---

## Estado

O cliente captura, codifica, conecta, transmite, recebe e desenha por conta
própria. Uma chamada acontece inteira aqui.

| | |
| --- | --- |
| Captura de tela — DXGI Desktop Duplication | ✅ |
| Captura de áudio — WASAPI process loopback, modo EXCLUDE | ✅ |
| Câmera — Media Foundation, composta no quadro pelo Video Processor | ✅ |
| Encoder H.264 por hardware — Media Foundation | ✅ |
| Sinalização — WebSocket | ✅ |
| Mídia — ICE/DTLS/SRTP sobre libdatachannel | ✅ |
| Recepção e desenho — decodificação por DXVA, Direct2D | ✅ |
| Interface — Direct2D, sem framework | ✅ |
| Captura de uma janela só, em vez do monitor inteiro | ⬜ |
| Controle de congestionamento que reage à rede | ⬜ |

As duas últimas linhas são o que o navegador ganha de graça com o libwebrtc e
aqui precisaria ser escrito à mão. O que existe hoje — pacer de RTP, jitter
buffer com correção de velocidade, anel sem trava entre a captura e o envio —
resolve o caso normal, e o motivo de cada escolha está no código.

---

## Sem placa de vídeo dedicada?

Funciona igual. Todo processador moderno traz gráficos integrados com encoder
de H.264 em hardware — Quick Sync na Intel, VCN na AMD — e é ele que o
`MFTEnumEx` acha primeiro.

Sem nenhum acelerador (máquina virtual, PC muito antigo), o cliente cai sozinho
para o encoder de software da Microsoft e para o rasterizador WARP. Continua
funcionando, custando CPU. Nesse caso vale escolher **720p** no painel: é a
diferença entre a máquina dar conta e não dar. Mesmo assim ele é mais leve que
o cliente em Electron, que carrega um Chromium inteiro antes de codificar o
primeiro quadro.

O painel diz em qual dos dois você está: a linha `captura` mostra `GPU` ou
`CPU`.

---

## Medido nesta máquina

Ryzen 5 1600, Radeon RX 590, monitor 1920x1080 com tela em movimento e som
tocando, 10 segundos:

```
VIDEO  (DXGI Desktop Duplication, 1920x1080)
  quadros entregues  : 591  (59.1/s)
  latencia present->captura: media 0.07 ms | p50 0.06 ms | p95 0.10 ms | max 0.16 ms
  quadros coalescidos: 0
  borda amarela      : nao

AUDIO  (WASAPI process loopback, modo EXCLUDE)
  excluido           : arvore do pid 10204 (discord)
  quadros capturados : 481440  (48111/s, esperado ~48000/s)
  descontinuidades   : 0
  reproducao         : 99.8% de audio real
```

O que importa não é a taxa — ela acompanha o refresh do monitor usado no teste —
e sim as duas linhas seguintes: **zero quadros coalescidos** e latência abaixo
de **0,2 ms no pior caso**. Coalescido zero significa que nenhum quadro
apresentado foi perdido entre uma leitura e outra; a latência baixa vem de a
duplicação entregar a textura que já está na GPU, sem passar pela memória
principal.

No áudio, o par que interessa é **zero descontinuidades** com a árvore do
Discord excluída: o processo é ignorado sem abrir buraco no que sobra.

A taxa que o GreenLabs transmite é a escolhida na interface (30 ou 60 por
segundo), não a da captura — o que a duplicação entrega além disso é descartado
antes de custar codificação.

---

## As três decisões que definem este cliente

### Só tela inteira, com DXGI. Sem borda amarela.

A captura de janela isolada usa Windows Graphics Capture, que **desenha uma
borda amarela** em volta da janela no Windows 10. Desligar isso depende de uma
API restrita do Windows 11.

A duplicação de área de trabalho não desenha nada: nenhuma borda, nenhuma
sobreposição, nenhum aviso. Por isso o cliente captura só tela inteira.

### Áudio: tudo MENOS o aplicativo escolhido

Modo **EXCLUDE**, não include. A captura pega jogo, Spotify, navegador e sons do
sistema, e deixa o Discord de fora. O Discord continua tocando normalmente nos
alto-falantes de quem transmite — ele só não entra na transmissão.

É exclusão na origem (`AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK` com
`PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE`), não mute. Ninguém fica
sem ouvir nada.

O Discord roda em vários processos, e o WASAPI exclui uma *árvore*. O
`ProcessTree` acha qual deles tem a sessão de áudio e sobe até a raiz — parando
assim que o pai deixa de ser um processo do Discord. **Essa parada é
essencial:** sem ela a subida chega ao `explorer.exe` e a árvore excluída passa
a ser a máquina inteira, o que fez o Discord nunca ser excluído e travou o PC na
versão 0.2.7 do cliente em Electron.

### A câmera vai composta no quadro, não numa segunda faixa

O servidor abre **um** transceiver de vídeo por pessoa, e monta o identificador
da faixa de saída a partir do dono mais o tipo. Duas faixas de vídeo do mesmo
dono colidiriam nesse identificador e a segunda seria descartada em silêncio.

Então a câmera entra pelo Video Processor, na mesma passada que converte a cor —
custo zero, porque a unidade de função fixa da placa já está lendo o quadro de
qualquer jeito. O efeito colateral é o melhor possível: quem assiste pelo
Electron ou pelo celular vê a câmera **hoje**, sem atualizar nada.

---

## Compilando

Precisa de **Visual Studio Build Tools 2022 ou mais novo** com o pacote
"Desenvolvimento para desktop com C++", o Windows SDK e CMake. DXGI, WASAPI,
Media Foundation e Direct2D vêm no próprio SDK; libdatachannel, Opus e mbedTLS
são buscados pelo CMake na primeira compilação.

```powershell
.\build.ps1                  # Release
.\build.ps1 -Config Debug
.\build.ps1 -Limpar          # apaga e reconfigura
```

O `build.ps1` monta `INCLUDE`, `LIB` e `PATH` na mão em vez de chamar o
`vcvars64.bat`. O vcvars encadeia uma sequência de `.bat` que trava em terminal
não interativo; como os caminhos são fixos, montar o ambiente direto é
equivalente e sempre termina.

Saída em `build\Release\bin\`. O link é estático (`/MT`): o executável roda em
máquina limpa, sem instalar o redistributable do Visual C++.

Para usar CMake direto, com o ambiente do MSVC já montado:

```powershell
cmake -S . -B build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release
```

---

## Medindo

O `greenlabs-probe` exercita o núcleo sem UI e sem rede. É a ferramenta de
comparação com o cliente em Electron.

```powershell
.\build\Release\bin\greenlabs-probe.exe --listar
.\build\Release\bin\greenlabs-probe.exe --segundos 10 --monitor 0
.\build\Release\bin\greenlabs-probe.exe --excluir discord,spotify
.\build\Release\bin\greenlabs-probe.exe --sem-video
```

Duas coisas para saber ao ler o resultado:

**"sem mudanca" não é perda.** A duplicação só entrega quadro quando a tela
muda. Com a área de trabalho parada o número vai a quase zero, e está certo.
Para medir vazão de verdade é preciso ter algo se movendo na tela.

**Para confirmar a exclusão do Discord**, toque som no Discord *e* em outro
aplicativo ao mesmo tempo. O pico de amplitude deve subir com o outro
aplicativo e **não** subir quando só o Discord estiver tocando.

O aplicativo também escreve tudo em `%LOCALAPPDATA%\GreenLabs\greenlabs.log` —
é o primeiro lugar para olhar quando algo não aparece.

---

## Estrutura

```
src/
├── capture/
│   ├── ScreenCapture    DXGI Desktop Duplication -> ID3D11Texture2D
│   ├── AudioCapture     WASAPI process loopback, modo EXCLUDE
│   ├── CameraCapture    Media Foundation, so video: microfone nunca e aberto
│   ├── Cursor           compoe o ponteiro no quadro capturado
│   └── ProcessTree      acha a raiz da arvore a excluir
├── video/
│   └── ColorConverter   BGRA <-> NV12, rotacao e a camera no canto
├── encoder/
│   └── VideoEncoder     H.264 por hardware, com queda para software
├── decoder/
│   └── VideoDecoder     H.264 por DXVA; um por transmissao recebida
├── audio/
│   ├── AudioCodec       Opus nos dois sentidos
│   ├── AudioPlayer      saida WASAPI, jitter buffer por velocidade, volume
│   └── RingBuffer       anel sem trava entre a captura e o envio
├── network/
│   ├── Signaling        WebSocket com o servidor
│   ├── Midia            ICE/DTLS/SRTP sobre libdatachannel
│   ├── Pacer            espalha os pacotes RTP no tempo
│   └── WebSocketClient
├── config/              preferencias em %APPDATA%\GreenLabs\config.json
├── main/
└── util/                log e JSON

ui/                      janela, Direct2D, tema
tools/probe/             medicao do nucleo, sem UI e sem rede
tools/sinal/             cliente de sinalizacao, para testar o servidor
```

O núcleo (`greenlabs_core`) não conhece a janela: `probe` e `sinal` usam as
mesmas peças sem abrir interface nenhuma. É o que permite medir captura e áudio
sem o desenho no meio.

---

## Números que vieram de medição, não de bom senso

Estão no código com o motivo junto. Mudá-los quebra coisas de um jeito difícil
de notar:

- **Teto adaptativo do anel de áudio** (`RingBuffer.h`): piso de 40 ms, crescendo
  até o dobro da maior rajada vista. Um teto fixo de 40 ms contra rajadas de
  60 ms fez o cliente em Electron tocar só **66%** do áudio, com o resto saindo
  como silêncio.
- **Buffer WASAPI de 200 ms** (`AudioCapture.cpp`): é o do exemplo oficial
  ApplicationLoopback. Já esteve em 5 segundos no capturador antigo — tamanho
  errado para tempo real, e latência de graça.
- **A parada na subida da árvore de processos** (`ProcessTree.cpp`): descrita
  acima.
- **Alvo de 80 ms no jitter buffer de áudio** (`AudioPlayer.cpp`): já esteve em
  60, e as faltas voltaram — cada falta é uma emenda audível. Menos folga é
  menos atraso só até o ponto em que vira estalo.
- **Captura no ritmo do fps escolhido, e não a cada volta do laço**
  (`Aplicacao.cpp`): o `AcquireNextFrame` e a composição do cursor rodando solto
  martelavam o dispositivo D3D centenas de vezes por segundo para produzir 30
  quadros úteis, e o decodificador do vídeo recebido brigava por ele.
- **`ID3D10Multithread::SetMultithreadProtected`** (`VideoDecoder.cpp`): sem
  isso o Media Foundation recusa a aceleração e decodifica na CPU **sem dizer
  nada**. Uma RX 590 parada enquanto o log dizia "Microsoft H264 Video Decoder
  MFT".
- **Nenhuma flag de ligação na textura do decodificador** (`VideoDecoder.cpp`):
  `D3D11_BIND_SHADER_RESOURCE` ali fazia o `CreateVideoProcessorInputView`
  recusar a textura com `E_INVALIDARG` em placa da AMD, quadro após quadro — era
  a razão de a tela dos outros nunca aparecer.
- **A qualidade é um teto, nunca uma meta** (`Aplicacao.cpp`): sem o limite em
  1x, um monitor 1024x768 no preset de 1080p era esticado para 1440x1080 — mais
  pixels para codificar, mais banda gasta, e a mesma imagem, borrada.
- **Captura e desenho na mesma thread** (`Aplicacao.cpp`): a proteção
  multithread torna cada *chamada* D3D atômica, não cada *sequência*. Em threads
  separadas, o compositor do cursor e o Direct2D se intercalam e o driver
  pendura a GPU — `DXGI_ERROR_DEVICE_HUNG` em segundos.
- **Uma thread de decodificação para todas as transmissões** (`Aplicacao.cpp`):
  uma por pessoa multiplicaria as sequências D3D concorrentes sobre o mesmo
  contexto imediato, que é o mesmo desenho que já pendurou a GPU antes.

---

## Licença

Mesmo licenciamento do projeto principal.
