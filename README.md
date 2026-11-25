# Inazuma Eleven 3 Recomp

Recompilação estática de **Inazuma Eleven 3 - Sekai e no Chousen!! The Ogre** (Nintendo DS) para rodar nativamente no PC.

> Este repositório **não contém** a ROM nem arquivos do jogo. Você precisa da sua própria cópia.
> ROM de referência: `BOEJ`, tradução BR T1.3, SHA-1 `a70e9a336f6ea8ff78003938dcd024533f034a68`.

## Status

Fase 2 em andamento: o jogo inteiro já é traduzido para C, compila e **roda no PC até o loop principal**, com threads, leitura de arquivos e save. O jogo já passa pelos créditos da tradução, pela **tela título** ("Ogros ao Ataque!"), pelo teclado do nome do save e entra no **começo da história**: conversas, mapa e o cenário 3D com o personagem andando. O 3D é um renderizador por software do hardware do DS (`runtime/gx.c`). Os vídeos Mobiclip ainda são pulados. Tem launcher com escolha da ROM e configurações (FPS, escala, tela cheia, layout). O som funciona: o lado do ARM7 (canais PCM/ADPCM/PSG, alarmes e mixagem) é HLE em `runtime/snd.c`, com saída pelo SDL.
Veja [docs/fase1.md](docs/fase1.md) (análise) e [docs/fase2.md](docs/fase2.md) (recompilador e runtime).

## Como jogar

```bash
make gen && make -j4 SDL=1     # precisa de SDL2 (Linux: libsdl2-dev)
./build/ie3                    # abre o launcher
./build/ie3 rom.nds --jogar    # ou direto, sem o launcher
```

O launcher segue o estilo dos launchers de recomp: menu lateral (Jogar, Gráficos, Controles, Sobre) e funciona com mouse ou teclado.
Na primeira vez ele pede a ROM: arraste o arquivo `.nds` para a janela (ou passe o caminho na linha de comando).
Ele confere se é a versão certa pelo SHA-1 e mostra a tela título do jogo, **gerada da sua ROM**: o launcher roda o próprio recomp sem janela até a tela título (`ie3 --capturar-titulo`) e guarda a imagem em `ie3_titulo_<sha>.ppm` ao lado do `.ini`. Nenhuma arte do jogo fica no repositório.
As opções ficam em `ie3recomp.ini`:

| Opção | Chave no .ini | Valores |
|---|---|---|
| ROM | `rom` | caminho do `.nds` |
| Limite de quadros | `fps` | 30, 60, 120, 144 ou 0 (sem limite) |
| Escala da janela | `escala` | 1 a 6 (2 = 512x768 com as telas em pé) |
| Tela cheia | `tela_cheia` | 0 ou 1 (F11 alterna durante o jogo) |
| Telas | `layout` | `vertical`, `lado_a_lado` ou `so_cima` (Tab troca para a de baixo) |
| Suavizar imagem | `suavizar` | 0 (pixels nítidos) ou 1 |
| V-Sync | `vsync` | 0 ou 1 |

Controles: setas, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter = START, Backspace = SELECT, mouse na tela de baixo = toque, Esc = sair.
Opções de linha de comando: `--fps`, `--escala`, `--tela-cheia`, `--config <arq>`, `--sem-janela` (testes); veja `./build/ie3 --help`.

O executável só lê a ROM que você fornece: o ARM9 vai para a memória comprimido, como no cartão, e o próprio código recompilado do jogo o descomprime; os overlays são lidos e descomprimidos direto da ROM.

## Como usar as ferramentas de análise

```bash
pip install -r requirements.txt
python3 -I tools/extract.py caminho/para/rom.nds extracted   # extrai ARM9, ITCM/DTCM, overlays e ARM7
python3 -I tools/analyze.py extracted analysis/functions.json  # descobre funções
python3 -I tools/dis.py 02029c08 40                            # disassembla um endereço
OV=16 python3 -I tools/xref.py 020f129c                        # início da função e quem chama (OV escolhe o overlay)
make gen && make -j4                                           # gera o C e compila build/ie3
RECOMP_WATCHDOG=20 ./build/ie3 rom.nds --sem-janela            # roda sem janela (testes)
tools/iterate.sh rom.nds                                       # roda e corrige funções faltantes automaticamente
```

## Plano

| Fase | Objetivo | Marco |
|---|---|---|
| 0 | Extrair e mapear a ROM | feito |
| 1 | Análise: funções, loop principal, overlays, IRQ, IPC | feito |
| 2 | Recompilador ARM/Thumb → C + runtime mínimo (2D) | telas de logo (em andamento: já chega ao loop principal) |
| 3 | Todos os overlays, arquivos e saves | menu principal |
| 4 | GPU 3D, som (HLE do ARM7), vídeos Mobiclip, entrada | partida jogável |
| 5 | Polimento (resolução, controles) | |
