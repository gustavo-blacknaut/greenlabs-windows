# GreenLabs — cliente nativo em C++

Migração do cliente [GreenLabs Live Streaming](https://github.com/gustavo-blacknaut/greenlabs-desktop)
de Electron para C++ nativo. Sem Electron, sem Chromium, sem Node.

**Estado: etapa 1 de 7.** O núcleo de captura está pronto e medido. Ainda não
há rede, encoder nem interface — o cliente em Electron continua sendo o que
transmite. Ver [ANALISE-MIGRACAO.md](ANALISE-MIGRACAO.md) para o plano completo
e os riscos.

---

## O que já funciona

| Etapa | Estado |
| --- | --- |
| 1. Captura de tela (DXGI) | ✅ pronto e medido |
| 1. Captura de áudio (WASAPI EXCLUDE) | ✅ pronto e medido |
| 2. Encoder | ⬜ |
| 3. Sinalização (WebSocket) | ⬜ |
| 4. Mídia (ICE/DTLS/SRTP) | ⬜ ← etapa de risco |
| 5. Recepção e renderização | ⬜ |
| 6. Interface | ⬜ |
| 7. Paridade e troca | ⬜ |

### Medido nesta máquina

Ryzen 5 1600, monitor 1920x1080 a 60 Hz, com tela em movimento e som tocando,
10 segundos:

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

59,1 quadros por segundo num monitor de 60 Hz é praticamente todo quadro
apresentado. A latência que a captura acrescenta fica abaixo de **0,2 ms no pior
caso** — a duplicação entrega a textura que já está na GPU, sem passar pela CPU.

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
"Desenvolvimento para desktop com C++" e o Windows SDK. Nada além disso: esta
etapa não usa nenhuma dependência externa — DXGI e WASAPI vêm no próprio SDK.

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
│   └── ProcessTree     acha a raiz da arvore a excluir
├── audio/
│   └── RingBuffer      buffer circular entre a thread de audio e o consumidor
└── util/
    └── Log

tools/probe/            medicao do nucleo, sem UI e sem rede
```

O núcleo (`greenlabs_core`) não conhece interface nem rede. Compila e roda
sozinho — é o que o `probe` prova, e é o que vai permitir medir latência sem a
janela no meio quando o encoder entrar.

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

Cada um desses deve virar teste antes da etapa 6.

---

## Licença

Mesmo licenciamento do projeto principal.
