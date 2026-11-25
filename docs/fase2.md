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

Ainda não há vídeo, som nem entrada: o jogo roda "às cegas". O próximo passo é o renderizador 2D para ver a primeira tela.

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
| `threads.c` | threads do SDK como fibras do host (`ucontext`), substituindo `OSi_RescheduleThread` |
| `thumb.c` | interpretador Thumb (o jogo só tem ~60 funções Thumb) |
| `main.c` | monta a memória como o bootloader deixaria e chama o `crt0` |

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
python3 -I tools/extract.py rom.nds extracted
python3 -I tools/analyze.py extracted analysis/functions.json
make gen && make -j4
RECOMP_WATCHDOG=20 ./build/ie3 extracted rom.nds
```

Variáveis úteis: `RECOMP_WATCHDOG=<s>` (mostra onde cada thread está se travar), `RECOMP_CARDLOG`, `RECOMP_PXILOG`, `RECOMP_IRQLOG`, `RECOMP_SAVELOG`, `RECOMP_SAVE=<arquivo>`.

## Limitações conhecidas

- Sem vídeo, som e entrada ainda.
- O tempo só anda no VBlank e no halt; não há limite de 60 quadros por segundo.
- O despacho trata "retorno" usando uma pilha de endereços de retorno; código que manipula a pilha de forma incomum pode confundir isso.
- 138 instruções sem suporte no código gerado (quase todas em "funções" falsas achadas por ponteiros em dados).
