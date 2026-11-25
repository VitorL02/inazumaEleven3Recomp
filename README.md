# Inazuma Eleven 3 Recomp

Recompilação estática de **Inazuma Eleven 3 - Sekai e no Chousen!! The Ogre** (Nintendo DS) para rodar nativamente no PC.

> Este repositório **não contém** a ROM nem arquivos do jogo. Você precisa da sua própria cópia.
> ROM de referência: `BOEJ`, tradução BR T1.3, SHA-1 `a70e9a336f6ea8ff78003938dcd024533f034a68`.

## Status

Fase 1 (análise do código). Veja [docs/fase1.md](docs/fase1.md).

## Como usar as ferramentas

```bash
pip install -r requirements.txt
python3 -I tools/extract.py caminho/para/rom.nds extracted   # extrai ARM9, ITCM/DTCM, overlays e ARM7
python3 -I tools/analyze.py extracted analysis/functions.json  # descobre funções
python3 -I tools/dis.py 02029c08 40                            # disassembla um endereço
OV=16 python3 -I tools/xref.py 020f129c                        # início da função e quem chama (OV escolhe o overlay)
```

## Plano

| Fase | Objetivo | Marco |
|---|---|---|
| 0 | Extrair e mapear a ROM | feito |
| 1 | Análise: funções, loop principal, overlays, IRQ, IPC | em andamento |
| 2 | Recompilador ARM/Thumb → C + runtime mínimo (2D) | telas de logo |
| 3 | Todos os overlays, arquivos e saves | menu principal |
| 4 | GPU 3D, som (HLE do ARM7), vídeos Mobiclip, entrada | partida jogável |
| 5 | Polimento (resolução, controles) | |
