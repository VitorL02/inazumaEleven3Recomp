#pragma once
#include <stdint.h>
#include "cpu.h"
/* Som: o lado do ARM7 do NitroSDK SND em HLE. Executa as listas de comandos que o ARM9 manda pelo
 * PXI (canais PCM/ADPCM/PSG/ruído, timers, volume, pan, alarmes) e mixa os 16 canais em 32768 Hz. */
void snd_run_commands(Cpu *c, uint32_t list);
/* chamado a cada quadro: avança os alarmes (devolve as mensagens PXI a mandar ao ARM9) e gera o áudio do quadro */
int snd_vblank(Cpu *c, uint32_t *alarm_msgs, int max);
/* lê amostras estéreo já mixadas (para a saída de áudio); devolve quantas havia */
int snd_read(int16_t *out, int frames);
#define SND_RATE 32768
/* modo acelerado (tecla de pular quadros): descarta o áudio que não cabe em ~60 ms de fila */
void snd_set_turbo(int on);
