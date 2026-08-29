# GreenLabs — cliente nativo em C++

Cliente nativo do [GreenLabs](https://github.com/gustavo-blacknaut/greenlabs-desktop),
escrito do zero em C++. Sem Electron, sem Chromium, sem Node — a janela é
Direct2D, a captura é DXGI, o codec é Media Foundation e o transporte é
libdatachannel.

**Estado: as sete etapas estão de pé.** O cliente captura, codifica, conecta,
transmite, recebe e desenha por conta própria. Uma chamada acontece inteira
aqui, sem Electron no caminho.

---

## O que funciona

| Etapa | Estado |
| --- | --- |
| 1. Captura de tela (DXGI Desktop Duplication) | ✅ |
| 1. Captura de áudio (WASAPI process loopback, modo EXCLUDE) | ✅ |
| 2. Encoder H.264 por hardware (Media Foundation) | ✅ |
| 3. Sinalização (WebSocket) | ✅ |
| 4. Mídia (ICE/DTLS/SRTP, via libdatachannel) | ✅ |
| 5. Recepção e renderização (decodificação por DXVA, Direct2D) | ✅ |
| 6. Interface (Direct2D, sem framework) | ✅ |
| 7. Paridade com o cliente em Electron | 🔸 quase |

O que ainda separa este cliente do de Electron é o que o navegador ganha de
graça com o libwebrtc e aqui foi preciso escrever à mão: controle de
congestionamento que reage à rede, e um jitter buffer no nível do NetEQ. O que
existe hoje — pacer de RTP, jitter buffer com correção de velocidade, anel sem
trava entre a captura e o envio — resolve o caso normal, e está documentado no
código junto com o motivo de cada escolha.

### O que este cliente faz que o de Electron não faz

- **Transmite o som do sistema inteiro menos um aplicativo.** O Discord fica de
  fora da captura sem ser silenciado para você. É process loopback do WASAPI em
  modo EXCLUDE, por árvore de processos — não existe equivalente no navegador.
- **Vê várias telas ao mesmo tempo.** Um decodificador por transmissão, com
  miniatura ao vivo de cada pessoa e troca de palco por clique.
- **Encoder e decodificador na GPU**, com a textura indo da duplicação para o
  encoder sem passar pela memória principal.
- **Corrige monitor girado** lendo a rotação do DXGI e girando no Video
  Processor, no mesmo passo da conversão de cor.

### Medido nesta máquina

Ryzen 5 1600, Radeon RX 590, captura de um monitor 1920x1080, com tela em
movimento e som tocando, 10 segundos:

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

O que importa nesses números não é a taxa — ela acompanha o refresh do monitor
usado no teste — e sim as duas linhas seguintes: **zero quadros coalescidos** e
latência abaixo de **0,2 ms no pior caso**. Coalescido zero significa que nenhum
quadro apresentado foi perdido entre uma leitura e outra; a latência baixa vem
de a duplicação entregar a textura que já está na GPU, sem passar pela memória
principal.

No áudio, o par que interessa é **zero descontinuidades** com a árvore do
Discord excluída: o processo é ignorado sem abrir buraco no que sobra.

A taxa que o GreenLabs realmente transmite é a escolhida na interface (30 ou
60 quadros por segundo), não a da captura — o que a duplicação entrega além
disso é descartado antes de custar codificação.

---

## As duas decisões que definem este cliente

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

Saída em `build\Release\bin\`.

Para usar CMake direto, sem o script, basta ter o ambiente do MSVC já montado:

```powershell
cmake -S . -B build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release
```

O link é estático (`/MT`): o executável roda em máquina limpa, sem instalar o
redistributable do Visual C++.

---

## Medindo

O `greenlabs-probe` exercita o núcleo sem UI e sem rede. É a ferramenta de
comparação com o cliente em Electron.

```powershell
.\build\Release\bin\greenlabs-probe.exe --listar
.\build\Release\bin\greenlabs-probe.exe --segundos 10 --monitor 0
.\build\Release\bin\greenlabs-probe.exe --excluir discord,spotify
.\build\Release\bin\greenlabs-probe.exe --sem-video    # so audio
```

Duas coisas para saber ao ler o resultado:

**"sem mudanca" não é perda.** A duplicação só entrega quadro quando a tela
muda. Com a área de trabalho parada o número de quadros vai a quase zero, e está
certo. Para medir vazão de verdade é preciso ter algo se movendo na tela.

**Para confirmar a exclusão do Discord**, toque som no Discord *e* em outro
aplicativo ao mesmo tempo. O pico de amplitude deve subir com o outro aplicativo
e **não** subir quando só o Discord estiver tocando.

---

## Estrutura

```
src/
├── capture/
│   ├── ScreenCapture   DXGI Desktop Duplication -> ID3D11Texture2D
│   ├── AudioCapture    WASAPI process loopback, modo EXCLUDE
│   ├── CursorCompositor compoe o ponteiro no quadro capturado
│   └── ProcessTree     acha a raiz da arvore a excluir
├── video/
│   └── ColorConverter  BGRA <-> NV12 e rotacao, no Video Processor
├── encoder/
│   └── VideoEncoder    H.264 por hardware, Media Foundation
├── decoder/
│   └── VideoDecoder    H.264 por DXVA; um por transmissao recebida
├── audio/
│   ├── AudioCodec      Opus nos dois sentidos
│   └── AudioPlayer     saida WASAPI com jitter buffer por velocidade
├── network/
│   ├── Signaling       WebSocket com o servidor
│   ├── Midia           ICE/DTLS/SRTP sobre libdatachannel
│   └── Pacer           espalha os pacotes RTP no tempo
├── config/
└── util/

ui/                     janela, Direct2D, tema
tools/probe/            medicao do nucleo, sem UI e sem rede
```

O núcleo (`greenlabs_core`) não conhece a janela: `probe` e `sinal` usam as
mesmas peças sem abrir interface nenhuma. É o que permite medir captura e
áudio sem o desenho no meio.

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
- **Captura e desenho na mesma thread** (`Aplicacao.cpp`): a proteção
  multithread torna cada *chamada* D3D atômica, não cada *sequência*. Em threads
  separadas, o compositor do cursor e o Direct2D se intercalam e o driver
  pendura a GPU — `DXGI_ERROR_DEVICE_HUNG` em segundos.

---

## Licença

Mesmo licenciamento do projeto principal.
