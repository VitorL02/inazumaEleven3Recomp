/* Som (ver snd.h). Os comandos seguem o driver do ARM7 do NitroSDK: o ARM9 monta listas de
 * SNDCommand {next, id, arg[4]} e manda o ponteiro pelo canal 7 do PXI. Aqui cada comando mexe
 * direto no estado dos 16 canais, e o mixador gera 32768 amostras estéreo por segundo. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snd.h"
#include "runtime.h"

enum { FMT_PCM8, FMT_PCM16, FMT_ADPCM, FMT_PSG };
typedef struct {
    int active, fmt, loop; /* loop: 1 = repete, 2 = uma vez */
    uint32_t src, loop_start, len; /* em bytes: início do laço e tamanho do laço */
    int vol, shift, pan, duty;
    uint32_t timer;
    double pos, step;      /* posição em amostras */
    int adpcm_sample, adpcm_index, adpcm_pos;   /* estado do ADPCM na amostra adpcm_pos */
    int loop_sample, loop_index, loop_saved;
    uint32_t lfsr; int psg_phase;
    int16_t cur;           /* última amostra decodificada */
    int cur_pos;
} Chan;
static Chan g_ch[16];
static int g_master = 127;
static uint32_t g_shared;
typedef struct { int active; uint32_t tick, period, left; int id; } Alarm;
static Alarm g_alarm[8];
static Cpu *g_c;

#define RING 16384
static int16_t g_ring[RING * 2];
static int g_turbo; /* acelerando: só mantém um pouco de áudio na fila, para não atrasar o som depois */
void snd_set_turbo(int on) { g_turbo = on; }
static volatile int g_rd, g_wr;
static FILE *g_wav; static uint32_t g_wav_n;

static const int16_t ima_step[89] = {7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
static const int8_t ima_idx[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static void set_step(Chan *ch) {
    /* o ARM7 grava 0x10000 - timer no SOUNDxTMR: o divisor do relógio de 16,76 MHz é o próprio timer */
    uint32_t div = ch->timer & 0xFFFF;
    if (!div) div = 1;
    ch->step = (33513982.0 / 2.0 / div) / SND_RATE;
    if (ch->fmt == FMT_PSG) ch->step *= 8; /* PSG: 8 passos por período */
}
static void chan_start(Chan *ch) {
    ch->active = 1; ch->pos = 0; ch->cur = 0; ch->cur_pos = -1; ch->loop_saved = 0;
    ch->lfsr = 0x7FFF; ch->psg_phase = 0;
    if (ch->fmt == FMT_ADPCM) {
        uint32_t h = rd32(g_c, ch->src);
        ch->adpcm_sample = (int16_t)(h & 0xFFFF); ch->adpcm_index = (h >> 16) & 0x7F;
        if (ch->adpcm_index > 88) ch->adpcm_index = 88;
        ch->adpcm_pos = 0;
    }
}
static int total_samples(const Chan *ch) {
    uint32_t bytes = ch->loop_start + ch->len;
    switch (ch->fmt) {
    case FMT_PCM8: return (int)bytes;
    case FMT_PCM16: return (int)(bytes / 2);
    case FMT_ADPCM: return (int)((bytes - 4) * 2);
    }
    return 0x7FFFFFFF;
}
static int loop_sample(const Chan *ch) {
    switch (ch->fmt) {
    case FMT_PCM8: return (int)ch->loop_start;
    case FMT_PCM16: return (int)(ch->loop_start / 2);
    case FMT_ADPCM: return ch->loop_start >= 4 ? (int)((ch->loop_start - 4) * 2) : 0;
    }
    return 0;
}
static int16_t adpcm_at(Chan *ch, int n) {
    /* decodifica até a amostra n (só anda para a frente; o laço volta ao estado salvo) */
    int ls = loop_sample(ch);
    if (n < ch->adpcm_pos) {
        if (ch->loop_saved && n >= ls) { ch->adpcm_sample = ch->loop_sample; ch->adpcm_index = ch->loop_index; ch->adpcm_pos = ls; }
        else chan_start(ch);
    }
    while (ch->adpcm_pos < n) {
        if (ch->adpcm_pos == ls && !ch->loop_saved) { ch->loop_sample = ch->adpcm_sample; ch->loop_index = ch->adpcm_index; ch->loop_saved = 1; }
        uint8_t b = (uint8_t)rd8(g_c, ch->src + 4 + (ch->adpcm_pos >> 1));
        int nib = (ch->adpcm_pos & 1) ? b >> 4 : b & 15;
        int step = ima_step[ch->adpcm_index];
        int diff = step >> 3;
        if (nib & 1) diff += step >> 2;
        if (nib & 2) diff += step >> 1;
        if (nib & 4) diff += step;
        int s = ch->adpcm_sample + ((nib & 8) ? -diff : diff);
        if (s > 32767) s = 32767; if (s < -32767) s = -32767;
        ch->adpcm_sample = s;
        ch->adpcm_index += ima_idx[nib & 7];
        if (ch->adpcm_index < 0) ch->adpcm_index = 0; if (ch->adpcm_index > 88) ch->adpcm_index = 88;
        ch->adpcm_pos++;
    }
    if (ch->adpcm_pos == ls && !ch->loop_saved) { ch->loop_sample = ch->adpcm_sample; ch->loop_index = ch->adpcm_index; ch->loop_saved = 1; }
    return (int16_t)ch->adpcm_sample;
}
static int chan_sample(Chan *ch, int idx) {
    if (ch->fmt == FMT_PSG) {
        int chno = (int)(ch - g_ch);
        if (chno >= 14) { /* ruído */
            int bit = ch->lfsr & 1; ch->lfsr >>= 1; if (bit) ch->lfsr ^= 0x6000;
            return bit ? -0x7FFF : 0x7FFF;
        }
        if (chno < 8) return 0;
        return (idx & 7) <= ch->duty ? -0x7FFF : 0x7FFF;
    }
    if (idx == ch->cur_pos) return ch->cur;
    int16_t v;
    if (ch->fmt == FMT_PCM8) v = (int16_t)((int8_t)rd8(g_c, ch->src + idx) << 8);
    else if (ch->fmt == FMT_PCM16) v = (int16_t)rd16(g_c, ch->src + idx * 2);
    else v = adpcm_at(ch, idx);
    ch->cur = v; ch->cur_pos = idx;
    return v;
}
static void mix(int frames) {
    static const int shifts[4] = {0, 1, 2, 4};
    for (int f = 0; f < frames; f++) {
        int64_t l = 0, r = 0;
        for (int i = 0; i < 16; i++) {
            Chan *ch = &g_ch[i];
            if (!ch->active) continue;
            int idx = (int)ch->pos;
            if (ch->fmt != FMT_PSG) {
                int tot = total_samples(ch);
                if (idx >= tot) {
                    if (ch->loop == 1 && ch->len) {
                        int ls = loop_sample(ch), ll = tot - ls;
                        if (ll <= 0) { ch->active = 0; continue; }
                        double over = ch->pos - tot;
                        ch->pos = ls + (over - (int)(over / ll) * ll);
                        idx = (int)ch->pos;
                    } else { ch->active = 0; continue; }
                }
            }
            int s = chan_sample(ch, idx);
            int v = (s >> shifts[ch->shift & 3]) * ch->vol; /* *127 */
            l += (int64_t)v * (128 - ch->pan);
            r += (int64_t)v * ch->pan;
            ch->pos += ch->step;
        }
        l = (l * g_master) >> 21; r = (r * g_master) >> 21; /* /127/128/127, ~x4 de ganho */
        if (l > 32767) l = 32767; if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; if (r < -32768) r = -32768;
        int nw = (g_wr + 1) % RING;
        if (nw != g_rd && (!g_turbo || (g_wr - g_rd + RING) % RING < 2048)) { g_ring[g_wr * 2] = (int16_t)l; g_ring[g_wr * 2 + 1] = (int16_t)r; g_wr = nw; }
        if (g_wav) { int16_t sm[2] = {(int16_t)l, (int16_t)r}; fwrite(sm, 2, 2, g_wav); g_wav_n++; }
    }
}
int snd_read(int16_t *out, int frames) {
    int n = 0;
    while (n < frames && g_rd != g_wr) { out[n * 2] = g_ring[g_rd * 2]; out[n * 2 + 1] = g_ring[g_rd * 2 + 1]; g_rd = (g_rd + 1) % RING; n++; }
    return n;
}

static void each(uint32_t mask, void (*fn)(Chan *, uint32_t, uint32_t), uint32_t a, uint32_t b) {
    for (int i = 0; i < 16; i++) if ((mask >> i) & 1) fn(&g_ch[i], a, b);
}
static void f_timer(Chan *ch, uint32_t t, uint32_t u) { (void)u; ch->timer = t; set_step(ch); }
static void f_vol(Chan *ch, uint32_t v, uint32_t s) { ch->vol = v & 0x7F; ch->shift = s & 3; }
static void f_pan(Chan *ch, uint32_t p, uint32_t u) { (void)u; ch->pan = p & 0x7F; }

/* channelStatus do SNDSharedWork (+8): canais tocando. O ARM7 atualiza logo depois de executar
 * cada lista de comandos; se só atualizasse no VBlank, o driver de som do jogo veria um canal
 * recém-iniciado (o áudio dos vídeos) como já terminado e o pararia. */
static void update_status(Cpu *c) {
    if (!g_shared) return;
    uint16_t st = 0;
    for (int i = 0; i < 16; i++) if (g_ch[i].active) st |= (uint16_t)(1 << i);
    wr16(c, g_shared + 8, st);
}
void snd_run_commands(Cpu *c, uint32_t cmd) {
    g_c = c;
    static int log = -1; if (log < 0) log = getenv("RECOMP_SNDLOG") != NULL;
    for (int n = 0; cmd && n < 4096; n++) {
        uint32_t id = rd32(c, cmd + 4), a0 = rd32(c, cmd + 8), a1 = rd32(c, cmd + 12), a2 = rd32(c, cmd + 16), a3 = rd32(c, cmd + 20);
        if (log) fprintf(stderr, "[snd] cmd %u %08x %08x %08x %08x\n", id, a0, a1, a2, a3);
        switch (id) {
        case 12: /* START_TIMER(canais, captura, alarmes, flags) */
            for (int i = 0; i < 16; i++) if ((a0 >> i) & 1) chan_start(&g_ch[i]);
            for (int i = 0; i < 8; i++) if ((a2 >> i) & 1) { g_alarm[i].active = 1; g_alarm[i].left = g_alarm[i].tick ? g_alarm[i].tick : g_alarm[i].period; }
            break;
        case 13: /* STOP_TIMER */
            for (int i = 0; i < 16; i++) if ((a0 >> i) & 1) g_ch[i].active = 0;
            for (int i = 0; i < 8; i++) if ((a2 >> i) & 1) g_alarm[i].active = 0;
            break;
        case 14: { /* SETUP_CHANNEL_PCM */
            Chan *ch = &g_ch[a0 & 15];
            ch->active = 0;
            ch->src = a1 & 0x7FFFFFF;
            ch->fmt = (a3 >> 24) & 3; ch->loop = (a3 >> 26) & 3;
            ch->loop_start = (a3 & 0xFFFF) * 4; ch->len = (a2 & 0x3FFFFF) * 4;
            ch->vol = (a2 >> 24) & 0x7F; ch->shift = (a2 >> 22) & 3;
            ch->timer = (a0 >> 16) & 0xFFFF; ch->pan = (a3 >> 16) & 0x7F;
            set_step(ch);
            break; }
        case 15: { /* SETUP_CHANNEL_PSG */
            Chan *ch = &g_ch[a0 & 15];
            ch->active = 0; ch->fmt = FMT_PSG; ch->duty = a3 & 7;
            ch->vol = a1 & 0x7F; ch->shift = (a1 >> 8) & 3;
            ch->timer = (a2 >> 8) & 0xFFFF; ch->pan = a2 & 0x7F;
            set_step(ch);
            break; }
        case 16: { /* SETUP_CHANNEL_NOISE */
            Chan *ch = &g_ch[a0 & 15];
            ch->active = 0; ch->fmt = FMT_PSG;
            ch->vol = a1 & 0x7F; ch->shift = (a1 >> 8) & 3;
            ch->timer = (a2 >> 8) & 0xFFFF; ch->pan = a2 & 0x7F;
            set_step(ch);
            break; }
        case 18: /* SETUP_ALARM(n, tick, período, id) */
            g_alarm[a0 & 7].tick = a1; g_alarm[a0 & 7].period = a2; g_alarm[a0 & 7].id = (int)(a3 & 0xFF);
            break;
        case 19: each(a0, f_timer, a1 & 0xFFFF, 0); break;
        case 20: each(a0, f_vol, a1, a2); break;
        case 21: each(a0, f_pan, a1, 0); break;
        case 23: g_master = a0 & 0x7F; break;
        case 29: g_shared = a0; break;
        case 33: /* READ_DRIVER_INFO: o ARM9 lê parte dos registradores dos canais */
            break;
        }
        cmd = rd32(c, cmd);
    }
    update_status(c);
}

int snd_vblank(Cpu *c, uint32_t *msgs, int max) {
    g_c = c;
    static int init;
    if (!init) {
        init = 1;
        const char *w = getenv("RECOMP_WAV");
        if (w && (g_wav = fopen(w, "wb"))) { uint8_t hdr[44] = {0}; fwrite(hdr, 1, 44, g_wav); }
    }
    /* 560190 ciclos de 33,5 MHz por quadro; os alarmes contam em ciclos/64 */
    static uint32_t frac;
    uint32_t ticks = (560190 + frac) / 64; frac = (560190 + frac) % 64;
    int n = 0;
    for (int i = 0; i < 8; i++) {
        Alarm *a = &g_alarm[i];
        if (!a->active || !a->period) continue;
        uint32_t t = ticks;
        while (t >= a->left) {
            t -= a->left; a->left = a->period;
            if (n < max) msgs[n++] = (uint32_t)(i | (a->id << 8));
        }
        a->left -= t;
    }
    /* áudio do quadro: 32768 / 59,8261 Hz */
    static double acc;
    acc += SND_RATE / 59.8261;
    int frames = (int)acc; acc -= frames;
    mix(frames);
    update_status(c);
    if (g_wav && (g_wav_n % SND_RATE) < (uint32_t)frames) { /* atualiza o cabeçalho a cada segundo */
        long here = ftell(g_wav);
        uint32_t data = g_wav_n * 4;
        uint8_t h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,0,0,0,0,0,0,0,0,4,0,16,0,'d','a','t','a',0,0,0,0};
        uint32_t v = 36 + data; memcpy(h + 4, &v, 4); v = SND_RATE; memcpy(h + 24, &v, 4); v = SND_RATE * 4; memcpy(h + 28, &v, 4); memcpy(h + 40, &data, 4);
        fseek(g_wav, 0, SEEK_SET); fwrite(h, 1, 44, g_wav); fseek(g_wav, here, SEEK_SET); fflush(g_wav);
    }
    return n;
}
