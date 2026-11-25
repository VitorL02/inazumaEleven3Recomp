# Fase 2: recompilador e runtime (protótipo)

## Estado atual

O jogo inteiro (ARM9 principal, ITCM e os 133 overlays) é traduzido para C e compilado num executável para PC.
Rodando no PC, o código recompilado:

- executa o `crt0` (cópia dos autoloads, BSS, construtores C++);
- faz o handshake com o ARM7 (simulado) e inicia o sistema operacional do SDK;
- cria e alterna **11 threads** do NitroSDK (cartão, som, carregamento de arquivos, jogo...);
- lê arquivos da ROM pelo código original do jogo (emulação dos registradores do cartão);
- lê o save (backup do cartão) a partir de um arquivo `ie3.sav`;
- carrega overlays (126, 130, 131, 16 e o 5, que é a primeira cena);
- **entra no loop principal do jogo** (`0202afd0`) e espera VBlank a cada quadro.

Com o renderizador 2D, **a primeira tela já aparece**: os créditos da tradução PT-BR (CyberLife, versão 1.3), com o fade-in do jogo.
A cena ainda não avança depois disso (investigação em andamento: provavelmente espera algo do ARM7, como o fim de um som).

## Como funciona

```
ROM ──tools/extract.py──> extracted/ (ARM9, ITCM, DTCM, overlays descomprimidos)
       tools/analyze.py ──> analysis/functions_list.txt (funções encontradas)
recomp/armrecomp.py ──> build/gen/*.c (uma função C por função ARM)
runtime/*.c + build/gen ──make──> build/ie3
```

### Tradutor (`recomp/armrecomp.py`)

- Decodifica ARMv5TE (processamento de dados, load/store, LDM/STM, multiplicações, CLZ, SMULxy, MRS/MSR, MCR/MRC, SWI).
- Cada instrução condicional vira `if (cond) { ... }`; saltos dentro da função viram `goto`.
- `BL` para função conhecida vira chamada C direta; chamadas para a área de overlays e chamadas indiretas passam por `rt_call()`.
- `ldr rX, [pc, #n]` vira constante (lida da ROM na tradução).
- Tabelas de salto (`addls pc, pc, rX, lsl #2`) viram `switch`.
- Os arquivos gerados são agrupados por módulo e bloco de 16 KB, para recompilar só o que mudou.
- `recomp/hle.txt` lista funções do jogo trocadas por código nativo.

### Runtime (`runtime/`)

| Arquivo | O que faz |
|---|---|
| `runtime.h` | acesso à memória (caminho rápido para a RAM principal), flags, deslocamentos |
| `runtime.c` | mapa de memória, I/O, divisor/raiz por hardware, DMA, timers, IRQ, cartão, PXI/ARM7 em HLE, save, despacho de funções, overlays |
| `threads.c` | threads do SDK como fibras do host (`hostfiber.c`: ucontext ou Fibers do Win32), substituindo `OSi_RescheduleThread` |
| `thumb.c` | interpretador Thumb (o jogo só tem ~60 funções Thumb) |
| `arm_interp.c` | interpretador ARM (ARMv5TE) para o decodificador Mobiclip do overlay 127 |
| `video.c` | bancos de VRAM e os dois motores 2D (BGs de texto/afins/bitmap, sprites, paletas estendidas, brilho mestre) |
| `main.c` | monta a memória como o bootloader deixaria, chama o `crt0`, grava quadros ou abre a janela |

### Funções substituídas (HLE)

| Endereço | Função | Por quê |
|---|---|---|
| `0200e580` | `FS_LoadOverlay` | usa a imagem já descomprimida e marca o overlay como ativo para o despacho |
| `0200e5d4` | `FS_UnloadOverlay` | idem |
| `0200e390` | `FS_StartOverlay` | overlays carregados de forma assíncrona |
| `02001728` | `OSi_RescheduleThread` | troca de thread precisa trocar também a pilha do C |
| `020020cc` | `OS_LoadContext` | idem, quando a thread atual não volta |

### ARM7 simulado (HLE)

O código do ARM7 não é traduzido. O runtime responde às mensagens do PXI como o ARM7 faria:

| Tag PXI | Uso | Resposta atual |
|---|---|---|
| sincronização (IPCSYNC) | handshake do `PXI_Init` | responde valor+1 |
| 6 | tela de toque | amostra "sem toque" em `02FFFFAA` |
| 7 | som | marca a lista de comandos como processada (sem áudio ainda) |
| 11 | save (backup) | leitura/escrita em `ie3.sav` |

### Funções descobertas em tempo de execução

Quando o código pula para um endereço que a análise estática não achou (por exemplo, a entrada no meio de uma rotina de divisão desenrolada), o runtime grava o endereço em `analysis/extra_functions.txt` e para. `tools/iterate.sh` regenera, recompila e tenta de novo automaticamente.

## Como rodar

```bash
python3 -I tools/extract.py rom.nds extracted        # só para gerar o código; o executável não usa extracted/
python3 -I tools/analyze.py extracted analysis/functions.json
make gen && make -j4 SDL=1
./build/ie3                                          # launcher
RECOMP_WATCHDOG=20 ./build/ie3 rom.nds --sem-janela   # testes
```

O executável lê só a ROM (`runtime/rom.c`): confere o código BOEJ e o SHA-1, copia o ARM9 comprimido para 02000000 e deixa o crt0 recompilado descomprimir e copiar os autoloads, como no aparelho. Os overlays saem da tabela de overlays da ROM, descomprimidos (BLZ) na primeira vez que são carregados.

Launcher (`runtime/launcher.c`, 960x540): desenha num buffer de pixels com fontes suavizadas embutidas (`runtime/fonts_aa.h`, DejaVu Sans, gerada por `tools/gen_fonts_aa.py`); fundo em redemoinho com raios gerado por código; o SDL2 só mostra o buffer e entrega teclado, mouse e arquivos arrastados. A prévia da tela título vem da ROM do usuário (`--capturar-titulo`, quadro 5400). A ROM é mapeada com `mmap`, não copiada. Configuração em `ie3recomp.ini` (`runtime/config.c`). Para testar sem janela: `RECOMP_LAUNCHER_DUMP=tela.ppm RECOMP_LAUNCHER_KEYS="drop=rom.nds,down,right,move=450:430,click=150:258" ./build/ie3` (com `RECOMP_LAUNCHER_WAIT_ART=30` espera a prévia).

Vídeo: `make SDL=1` abre uma janela (precisa de SDL2), com limite de quadros (`fps` no .ini), escala, tela cheia e layout das telas. Sem janela, `RECOMP_DUMP=<pasta>` grava quadros PPM a cada `RECOMP_DUMP_EVERY` quadros.
Entrada por roteiro: `RECOMP_INPUT="150-155:A;300-305:TOUCH=128,96;400:START"` (quadros e teclas A B SELECT START RIGHT LEFT UP DOWN R L X Y).

Variáveis úteis: `RECOMP_PROFILE=1` (com o watchdog, mostra onde o tempo é gasto), `RECOMP_WATCHDOG=<s>` (mostra onde cada thread está se travar), `RECOMP_CARDLOG`, `RECOMP_PXILOG`, `RECOMP_IRQLOG`, `RECOMP_SAVELOG`, `RECOMP_SAVE=<arquivo>`.

## Da tela de créditos à tela título (25/11)

- **Contador de VBlank:** o ARM7 incrementa 02FFFC3C a cada quadro, e o laço principal (0202b13c) usa a diferença dele como "quadros passados". Sem isso nenhuma contagem de tempo andava. Agora o runtime incrementa o contador no `rt_vblank`.
- **Tabelas de salto** (`add pc, pc, rX, lsl #2`): o tamanho vem do maior `cmp rX, #n` antes da instrução, e as entradas podem ser `pop` além de `b`.
- **Funções que pulam para trás da entrada** (código compartilhado, ex.: divisão de 64 bits em 0201f954 → 0201f818): o C sai em ordem de endereço, então a função começa com `goto` para a entrada. Afetava 26 funções.
- **Chamadas montadas à mão** (`add lr, pc, #0` ou `mov lr, pc` seguido de `bx rX`) viram chamadas.
- **Vídeos Mobiclip** (overlay 127): o decodificador é assembly escrito à mão com saltos calculados (`add pc, r6, r5`) e chamadas montadas com `add lr, pc`. Ele roda no interpretador ARM (`runtime/arm_interp.c`): `rt_call` manda para lá qualquer endereço do overlay 127 enquanto ele está carregado, e o interpretador chama o código recompilado quando sai da região. O DS roda o ARM9 a 67 MHz, então interpretar esse trecho é rápido (a abertura decodifica a mais de 400 quadros por segundo sem janela). O som dos vídeos usa os canais 8 e 9 com alarme; o runtime atualiza o `channelStatus` logo depois de cada lista de comandos, como o ARM7, senão o driver do jogo para esses canais.
- **Tarefas cooperativas do jogo:** até 16 corrotinas com pilha própria dentro da thread principal (0206efd8 retoma, 0206ee5c cede, 0206ede4 termina; tabelas em 020b9d4c..020b9d94). Cada tarefa vira uma fibra do host (`runtime/threads.c`).
- `RECOMP_SPCHECK=1` avisa quando uma função chamada por `rt_call` devolve `sp` diferente: foi assim que os dois problemas acima apareceram.

Próximo: depois de "Novo Jogo" aparece o aviso de save adulterado; ver por que o save (ie3.sav vazio) é considerado inválido e como o aviso é fechado.

## Limitações conhecidas

- 3D por software (`runtime/gx.c`): pilhas de matrizes, luz, recorte, os 7 formatos de textura, translucidez,
  neblina, testes de caixa/posição. Faltam polígonos de sombra, marcação de bordas e antisserrilhado.
- O envio assíncrono de comandos 3D (MI_SendGXCommandAsync) depende da IRQ do GXFIFO, que aqui é por nível
  (o FIFO está sempre vazio); `rt_check_irq` repete enquanto houver IRQ pendente.
- Som (`runtime/snd.c`): o jogo tem um driver de som próprio no ARM9 que usa os canais do SND diretamente
  (LOCK_CHANNEL em todos) e é acordado por um alarme do ARM7 a cada 10 ms. O HLE executa as listas de
  comandos (SETUP_CHANNEL_PCM/PSG/NOISE, START/STOP_TIMER, CHANNEL_TIMER/VOLUME/PAN, SETUP_ALARM), manda os
  alarmes pelo PXI (alarme | id << 8, canal 7), mantém o channelStatus do SNDSharedWork e mixa em 32768 Hz.
  O timer do comando já é o divisor (o ARM7 grava 0x10000 - timer). RECOMP_WAV=arquivo.wav grava a saída;
  RECOMP_MUDO desliga o áudio.
- Sem janelas no vídeo 2D (o blending é simples).
- O tempo só anda no VBlank e no halt; o ritmo de 60 quadros por segundo vem do limitador da janela.
- Windows: `make windows` (MinGW) gera `build-win/ie3.exe`; o que muda por sistema fica em `platform.c` e `hostfiber.c`.
  O .exe compila, mas ainda não foi executado num Windows de verdade (o Wine não roda neste ambiente).
- O despacho trata "retorno" usando uma pilha de endereços de retorno; código que manipula a pilha de forma incomum pode confundir isso.
- 138 instruções sem suporte no código gerado (quase todas em "funções" falsas achadas por ponteiros em dados).
