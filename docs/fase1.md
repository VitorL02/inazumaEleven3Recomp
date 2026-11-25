# Fase 1: análise do código

Tudo aqui saiu das ferramentas em `tools/` rodando sobre a ROM de referência.
Endereços são de RAM do ARM9. Quando o endereço está numa região de overlay, o overlay é indicado (`ovNNN`), porque vários overlays ocupam os mesmos endereços.

## Layout

| Módulo | RAM | Tamanho |
|---|---|---|
| ARM9 principal | `02000000`–`02093920` | 604.448 B |
| ITCM (autoload) | `01FF8000` | 15.648 B |
| DTCM (autoload) | `027E0000` | 96 B + 32 B bss |
| Overlays (133) | `020BCB40`–`021604C0` | 2,7 MB, 6 slots |
| ARM7 | `02380000` | 153 KB |

Área compartilhada do sistema em `02FFFxxx` (layout dos SDKs da época do DSi, ex.: cabeçalho da ROM em `02FFFE00`), e não em `027FFxxx`. O runtime precisa preencher essa área como o BIOS/firmware faria.

## Caminho de boot

| Endereço | O que é | Como sei |
|---|---|---|
| `02000800` | `_start` (crt0): inicializa pilhas, autoloads, BSS | entry do cabeçalho |
| `02000c8c` | `NitroMain` (veneer: `ldr ip,=02029c08; bx ip`) | literal em `02000934`, chamada via `bx r1` com `lr=FFFF0000` |
| `02029c08` | main real: ~20 chamadas de init, depois o jogo | disassembly |
| `02029c34` | carrega o **overlay 126** no boot | `mov r0,#0; ldr r1,=126; bl FS_LoadOverlay` |
| `020290f0`/`02029128` | carrega o **overlay 130** (e de novo depois) | literal `130` em `02029130` |
| `0202afd0` | candidato a loop principal do jogo (chamado no fim do main) | chama input, overlay 16 (`020f129c`) e vários subsistemas |

## Sistema de overlays

| Endereço | Função (NitroSDK) | Evidência |
|---|---|---|
| `0200ff80` | `CARD_GetRomHeader` → `02FFFE00` | retorna literal |
| `0200e0f0` | `FS_LoadOverlayInfo` | lê cabeçalho+`0x50` (ARM9) ou `+0x58` (ARM7), compara id com tamanho da tabela, lê 32 bytes |
| `0200e580` | `FS_LoadOverlay(target, id)` | LoadOverlayInfo → LoadOverlayImage (`0200e250`) → StartOverlay (`0200e390`) |
| `0200e5d4` | `FS_UnloadOverlay(target, id)` | mesma forma, chama `0200e570` |
| `0202e520` | gerenciador de overlay do jogo (descarrega e carrega um par) | chama Unload e Load em sequência |

Chamadores de `FS_LoadOverlay` / `FS_UnloadOverlay`: o main, `0202e520`/`0202eb00`, e os overlays 15, 16 e 131. Ou seja, a troca de cenas é feita em poucos lugares, o que é bom para o runtime.

## Funções encontradas

Descida recursiva + varredura de prólogos (`push {...,lr}`):

| Módulo | Funções |
|---|---|
| ARM9 principal | 3.364 (só 6 em Thumb) |
| ITCM | 45 |
| Overlays | ~10.900 |
| **Total** | **~14.300** |

É um limite inferior: funções folha sem `push lr` e chamadas só por ponteiro ainda podem faltar. O código é quase todo ARM (compilado em C++ com o CodeWarrior/MWCC, com vtables, ex.: `0208f6e0`).

## Chamadas do ARM9 principal para dentro dos overlays

O código fixo chama **376 endereços** dentro da área de overlays (734 chamadas). Atribuindo pelo início de função:

| Overlay | Alvos |
|---|---|
| ov016 | 205 |
| ov132 | 26 |
| ov126 | 13 |
| ov127 | 8 |
| outros | 15 |
| ambíguo (vários overlays têm função no mesmo endereço) | 7 |
| sem função conhecida | 102 |

Conclusão: **o overlay 16 funciona como parte fixa do jogo** (motor), e o 126 e o 130 sobem no boot.

## Consequências para o recompilador

1. **Função = (módulo, endereço)**, não só endereço. Dois overlays podem ter funções diferentes no mesmo endereço. Chamadas para a área de overlays precisam de um despachante que olha qual overlay está carregado em cada slot.
2. O runtime intercepta `FS_LoadOverlay`/`FS_UnloadOverlay` (`0200e580`/`0200e5d4`) para trocar a tabela de funções ativa, em vez de copiar código.
3. Chamadas indiretas (`blx rX`, vtables C++) são muitas: precisa de uma tabela endereço→função gerada para todos os módulos, com fallback de erro.
4. Quase tudo é ARM; Thumb é raro (facilita o primeiro protótipo).
5. A área `02FFFxxx` e o cabeçalho em `02FFFE00` precisam ser montados pelo runtime.

## Ainda não verificado

- Se `0202afd0` é mesmo o loop principal (falta ver o laço e o VBlank).
- Rotinas de IRQ/VBlank e a comunicação ARM9↔ARM7 (FIFO).
- As 102 chamadas para endereços sem função conhecida.
- Comparação com o dqix-decomp (o build do ds-decomp foi bloqueado neste ambiente; a análise usou ferramentas próprias em Python).
