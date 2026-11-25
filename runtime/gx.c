/* Motor 3D do DS por software (ver gx.h).
 * Geometria em ponto fixo como no hardware (matrizes 20.12, vértices 4.12), recorte e rasterização em
 * ponto flutuante com interpolação de perspectiva. Cobre: pilhas de matrizes, os 4 tipos de primitiva,
 * luz, matriz de textura, os 7 formatos de textura, modulação/decalque/toon/realce, translucidez,
 * teste de alfa, Z/W-buffer, neblina e os testes de caixa/posição/vetor. Ainda não há polígonos de
 * sombra, marcação de bordas, antisserrilhado nem a imagem de fundo do plano traseiro. */
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include "gx.h"
#include "video.h"

uint16_t g_gx_color[256 * 192];
uint8_t g_gx_alpha[256 * 192];

typedef struct { int32_t m[16]; } Mtx; /* linha maior: m[l*4+c], 20.12 */

static Mtx g_proj, g_pos, g_vec, g_tex, g_clip;
static Mtx g_proj_stack[1], g_pos_stack[32], g_vec_stack[32], g_tex_stack[1];
static int g_proj_sp, g_pos_sp, g_tex_sp, g_mtx_mode, g_stack_err;

/* estado de vértice */
static int32_t g_vx, g_vy, g_vz;
static int g_r = 31, g_g = 31, g_b = 31;
static int32_t g_raw_s, g_raw_t, g_s, g_t;
static uint32_t g_poly_attr, g_poly_attr_pending, g_tex_param, g_pltt_base;
static uint32_t g_dif_amb, g_spe_emi;
static int32_t g_light_vec[4][3];
static uint16_t g_light_col[4];
static uint8_t g_shininess[128];
static int g_prim = -1, g_prim_count;
static uint32_t g_viewport = 0xBFFF0000;
static int32_t g_pos_result[4];
static int16_t g_vec_result[3];
static int g_box_result;

/* ---------- lista de polígonos ---------- */
typedef struct { float x, y, z, w, s, t, r, g, b; } Vtx;
typedef struct {
    Vtx v[10]; int n;
    uint32_t attr, tex, pltt;
    int translucent, wbuffer;
    float ymin;
    uint32_t viewport;
} Poly;
#define MAX_POLYS 4096
static Poly g_polys[2][MAX_POLYS];
static int g_npolys[2], g_cur; /* g_cur: lista sendo montada */
static int g_swap_pending; static uint32_t g_swap_param;
static Vtx g_strip[4]; static int g_strip_n;

static void mtx_identity(Mtx *m) { memset(m, 0, sizeof *m); m->m[0] = m->m[5] = m->m[10] = m->m[15] = 1 << 12; }
static void mtx_mul(Mtx *out, const Mtx *a, const Mtx *b) { /* out = a * b */
    Mtx r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            int64_t s = 0;
            for (int k = 0; k < 4; k++) s += (int64_t)a->m[i * 4 + k] * b->m[k * 4 + j];
            r.m[i * 4 + j] = (int32_t)(s >> 12);
        }
    *out = r;
}
static void update_clip(void) { mtx_mul(&g_clip, &g_pos, &g_proj); }

/* aplica `p * atual` às matrizes que o modo atual afeta */
static void mtx_apply(const Mtx *p, int is_scale) {
    switch (g_mtx_mode) {
    case 0: mtx_mul(&g_proj, p, &g_proj); break;
    case 1: mtx_mul(&g_pos, p, &g_pos); break;
    case 2: mtx_mul(&g_pos, p, &g_pos); if (!is_scale) mtx_mul(&g_vec, p, &g_vec); break;
    case 3: mtx_mul(&g_tex, p, &g_tex); break;
    }
    if (g_mtx_mode < 3) update_clip();
}
static void mtx_load(const Mtx *p) {
    switch (g_mtx_mode) {
    case 0: g_proj = *p; break;
    case 1: g_pos = *p; break;
    case 2: g_pos = *p; g_vec = *p; break;
    case 3: g_tex = *p; break;
    }
    if (g_mtx_mode < 3) update_clip();
}

/* ---------- luz ---------- */
static int clamp31(int v) { return v < 0 ? 0 : v > 31 ? 31 : v; }
static void do_lighting(int32_t nx, int32_t ny, int32_t nz) {
    /* normal transformada pela matriz de vetores (só 3x3) */
    int32_t n[3];
    for (int j = 0; j < 3; j++) n[j] = (int32_t)(((int64_t)nx * g_vec.m[0 * 4 + j] + (int64_t)ny * g_vec.m[1 * 4 + j] + (int64_t)nz * g_vec.m[2 * 4 + j]) >> 12);
    int dr = g_dif_amb & 31, dg = (g_dif_amb >> 5) & 31, db = (g_dif_amb >> 10) & 31;
    int ar = (g_dif_amb >> 16) & 31, ag = (g_dif_amb >> 21) & 31, ab = (g_dif_amb >> 26) & 31;
    int sr = g_spe_emi & 31, sg = (g_spe_emi >> 5) & 31, sb = (g_spe_emi >> 10) & 31;
    int er = (g_spe_emi >> 16) & 31, eg = (g_spe_emi >> 21) & 31, eb = (g_spe_emi >> 26) & 31;
    int use_table = (g_spe_emi >> 15) & 1;
    int r = er << 12, g = eg << 12, b = eb << 12;
    for (int l = 0; l < 4; l++) {
        if (!((g_poly_attr >> l) & 1)) continue;
        int32_t *L = g_light_vec[l];
        int lr = g_light_col[l] & 31, lg = (g_light_col[l] >> 5) & 31, lb = (g_light_col[l] >> 10) & 31;
        int32_t dif = -(int32_t)(((int64_t)L[0] * n[0] + (int64_t)L[1] * n[1] + (int64_t)L[2] * n[2]) >> 12);
        if (dif < 0) dif = 0; if (dif > 4096) dif = 4096;
        int32_t h[3] = {L[0] / 2, L[1] / 2, (L[2] - 4096) / 2};
        int32_t sh = -(int32_t)(((int64_t)h[0] * n[0] + (int64_t)h[1] * n[1] + (int64_t)h[2] * n[2]) >> 12);
        if (sh < 0) sh = 0; if (sh > 4096) sh = 4096;
        sh = (int32_t)(((int64_t)sh * sh) >> 12);
        if (use_table) sh = g_shininess[(sh >> 5) & 127] << 4;
        r += (sr * lr * sh) / 31 + (dr * lr * dif) / 31 + ((ar * lr) << 12) / 31;
        g += (sg * lg * sh) / 31 + (dg * lg * dif) / 31 + ((ag * lg) << 12) / 31;
        b += (sb * lb * sh) / 31 + (db * lb * dif) / 31 + ((ab * lb) << 12) / 31;
    }
    g_r = clamp31(r >> 12); g_g = clamp31(g >> 12); g_b = clamp31(b >> 12);
}

/* ---------- montagem de polígonos ---------- */
static void tex_transform_vertex(void) {
    if ((g_tex_param >> 30) == 3) {
        g_s = (int32_t)(((int64_t)g_vx * g_tex.m[0] + (int64_t)g_vy * g_tex.m[4] + (int64_t)g_vz * g_tex.m[8]) >> 24) + g_raw_s;
        g_t = (int32_t)(((int64_t)g_vx * g_tex.m[1] + (int64_t)g_vy * g_tex.m[5] + (int64_t)g_vz * g_tex.m[9]) >> 24) + g_raw_t;
    }
}
static int clip_poly(Vtx *in, int n, int *far_cut) {
    /* Sutherland-Hodgman contra -w<=x,y,z<=w */
    Vtx buf[2][12]; Vtx *src = in; int sn = n;
    for (int plane = 0; plane < 6; plane++) {
        Vtx *dst = buf[plane & 1]; int dn = 0;
        int axis = plane >> 1; float sgn = (plane & 1) ? -1.f : 1.f;
        for (int i = 0; i < sn; i++) {
            Vtx *a = &src[i], *b = &src[(i + 1) % sn];
            float ca = (axis == 0 ? a->x : axis == 1 ? a->y : a->z) * sgn, cb = (axis == 0 ? b->x : axis == 1 ? b->y : b->z) * sgn;
            float da = a->w - ca, db = b->w - cb; /* >= 0 dentro */
            if (da >= 0) { if (dn < 12) dst[dn++] = *a; }
            if ((da >= 0) != (db >= 0)) {
                if (axis == 2 && plane == 4) *far_cut = 1;
                float t = da / (da - db);
                Vtx v;
                v.x = a->x + (b->x - a->x) * t; v.y = a->y + (b->y - a->y) * t; v.z = a->z + (b->z - a->z) * t; v.w = a->w + (b->w - a->w) * t;
                v.s = a->s + (b->s - a->s) * t; v.t = a->t + (b->t - a->t) * t;
                v.r = a->r + (b->r - a->r) * t; v.g = a->g + (b->g - a->g) * t; v.b = a->b + (b->b - a->b) * t;
                if (dn < 12) dst[dn++] = v;
            }
        }
        src = dst; sn = dn;
        if (sn < 3) return 0;
    }
    if (sn > 10) sn = 10;
    memcpy(in, src, sn * sizeof(Vtx));
    return sn;
}
static void submit_poly(Vtx *v, int n) {
    /* descarte de face (no espaço de recorte, como o hardware) */
    Vtx *v0 = &v[0], *v1 = &v[1], *v2 = &v[2];
    double nx = (double)v0->y * v2->w - (double)v0->w * v2->y;
    double ny = (double)v0->w * v2->x - (double)v0->x * v2->w;
    double nz = (double)v0->x * v2->y - (double)v0->y * v2->x;
    double dot = v1->x * nx + v1->y * ny + v1->w * nz;
    uint32_t attr = g_poly_attr;
    if (dot < 0) { if (!(attr & (1 << 7))) return; }
    else if (dot > 0) { if (!(attr & (1 << 6))) return; }
    else if (!(attr & (3 << 6))) return;
    if (((attr >> 4) & 3) == 3) return; /* sombra: ainda não */
    if (g_npolys[g_cur] >= MAX_POLYS) return;
    Poly *p = &g_polys[g_cur][g_npolys[g_cur]];
    memcpy(p->v, v, n * sizeof(Vtx));
    int far_cut = 0;
    int cn = clip_poly(p->v, n, &far_cut);
    if (!cn) return;
    if (far_cut && !(attr & (1 << 12))) return;
    p->n = cn; p->attr = attr; p->tex = g_tex_param; p->pltt = g_pltt_base; p->viewport = g_viewport;
    int alpha = (attr >> 16) & 31, fmt = (g_tex_param >> 26) & 7;
    p->translucent = (alpha != 31 && alpha != 0) || fmt == 1 || fmt == 6;
    /* para a tela: x,y em pixels, z = profundidade 0..1, w guardado; atributos divididos por w */
    int x1 = g_viewport & 0xFF, y1 = (g_viewport >> 8) & 0xFF, x2 = (g_viewport >> 16) & 0xFF, y2 = (g_viewport >> 24) & 0xFF;
    float vw = (float)(x2 - x1 + 1), vh = (float)(y2 - y1 + 1);
    p->ymin = 1e9f;
    for (int i = 0; i < cn; i++) {
        Vtx *q = &p->v[i];
        float w = q->w != 0 ? q->w : 1e-6f, iw = 1.0f / w;
        q->x = (q->x * iw + 1.0f) * vw * 0.5f + x1;
        q->y = (1.0f - q->y * iw) * vh * 0.5f + (191 - y2);
        q->z = (q->z * iw) * 0.5f + 0.5f;
        q->w = w / 4096.0f;
        if (q->y < p->ymin) p->ymin = q->y;
    }
    g_npolys[g_cur]++;
}
static void add_vertex(void) {
    if (g_prim < 0) return;
    tex_transform_vertex();
    Vtx v;
    int32_t in[4] = {g_vx, g_vy, g_vz, 1 << 12}, out[4];
    for (int j = 0; j < 4; j++) {
        int64_t s = 0;
        for (int i = 0; i < 4; i++) s += (int64_t)in[i] * g_clip.m[i * 4 + j];
        out[j] = (int32_t)(s >> 12);
    }
    v.x = (float)out[0]; v.y = (float)out[1]; v.z = (float)out[2]; v.w = (float)out[3];
    v.s = (float)g_s; v.t = (float)g_t;
    v.r = (float)g_r; v.g = (float)g_g; v.b = (float)g_b;
    g_strip[g_strip_n++] = v;
    g_prim_count++;
    Vtx poly[4];
    switch (g_prim) {
    case 0: /* triângulos */
        if (g_strip_n == 3) { memcpy(poly, g_strip, 3 * sizeof(Vtx)); submit_poly(poly, 3); g_strip_n = 0; }
        break;
    case 1: /* quadriláteros */
        if (g_strip_n == 4) { memcpy(poly, g_strip, 4 * sizeof(Vtx)); submit_poly(poly, 4); g_strip_n = 0; }
        break;
    case 2: /* faixa de triângulos */
        if (g_strip_n == 3) {
            if (g_prim_count & 1) { poly[0] = g_strip[0]; poly[1] = g_strip[1]; poly[2] = g_strip[2]; }
            else { poly[0] = g_strip[1]; poly[1] = g_strip[0]; poly[2] = g_strip[2]; }
            submit_poly(poly, 3);
            g_strip[0] = g_strip[1]; g_strip[1] = g_strip[2]; g_strip_n = 2;
        }
        break;
    case 3: /* faixa de quadriláteros */
        if (g_strip_n == 4) {
            poly[0] = g_strip[0]; poly[1] = g_strip[1]; poly[2] = g_strip[3]; poly[3] = g_strip[2];
            submit_poly(poly, 4);
            g_strip[0] = g_strip[2]; g_strip[1] = g_strip[3]; g_strip_n = 2;
        }
        break;
    }
}

/* ---------- testes ---------- */
static void box_test(const int32_t *p) {
    int32_t x = (int16_t)p[0], y = (int16_t)(p[0] >> 16), z = (int16_t)p[1];
    int32_t w = (int16_t)(p[1] >> 16), h = (int16_t)p[2], d = (int16_t)(p[2] >> 16);
    int out_all[6] = {1, 1, 1, 1, 1, 1};
    for (int i = 0; i < 8; i++) {
        int32_t in[4] = {x + ((i & 1) ? w : 0), y + ((i & 2) ? h : 0), z + ((i & 4) ? d : 0), 1 << 12}, o[4];
        for (int j = 0; j < 4; j++) { int64_t s = 0; for (int k = 0; k < 4; k++) s += (int64_t)in[k] * g_clip.m[k * 4 + j]; o[j] = (int32_t)(s >> 12); }
        if (o[0] <= o[3]) out_all[0] = 0;
        if (o[0] >= -o[3]) out_all[1] = 0;
        if (o[1] <= o[3]) out_all[2] = 0;
        if (o[1] >= -o[3]) out_all[3] = 0;
        if (o[2] <= o[3]) out_all[4] = 0;
        if (o[2] >= -o[3]) out_all[5] = 0;
    }
    g_box_result = !(out_all[0] || out_all[1] || out_all[2] || out_all[3] || out_all[4] || out_all[5]);
}

/* ---------- comandos ---------- */
static const int8_t g_nparams[256] = {
    [0x10] = 1, [0x11] = 0, [0x12] = 1, [0x13] = 1, [0x14] = 1, [0x15] = 0, [0x16] = 16, [0x17] = 12,
    [0x18] = 16, [0x19] = 12, [0x1A] = 9, [0x1B] = 3, [0x1C] = 3,
    [0x20] = 1, [0x21] = 1, [0x22] = 1, [0x23] = 2, [0x24] = 1, [0x25] = 1, [0x26] = 1, [0x27] = 1, [0x28] = 1,
    [0x29] = 1, [0x2A] = 1, [0x2B] = 1, [0x30] = 1, [0x31] = 1, [0x32] = 1, [0x33] = 1, [0x34] = 32,
    [0x40] = 1, [0x41] = 0, [0x50] = 1, [0x60] = 1, [0x70] = 3, [0x71] = 2, [0x72] = 1,
};
static int valid_cmd(int c) { return c == 0x11 || c == 0x15 || c == 0x41 || g_nparams[c] > 0; }

static void exec_cmd(int cmd, const int32_t *p) {
    Mtx m;
    switch (cmd) {
    case 0x10: g_mtx_mode = p[0] & 3; break;
    case 0x11: /* PUSH */
        if (g_mtx_mode == 0) { if (g_proj_sp > 0) g_stack_err = 1; g_proj_stack[0] = g_proj; g_proj_sp = 1; }
        else if (g_mtx_mode == 3) { g_tex_stack[0] = g_tex; g_tex_sp = 1; }
        else { if (g_pos_sp >= 31) g_stack_err = 1; g_pos_stack[g_pos_sp & 31] = g_pos; g_vec_stack[g_pos_sp & 31] = g_vec; g_pos_sp = (g_pos_sp + 1) & 63; }
        break;
    case 0x12: { /* POP */
        if (g_mtx_mode == 0) { g_proj_sp = 0; g_proj = g_proj_stack[0]; update_clip(); }
        else if (g_mtx_mode == 3) { g_tex_sp = 0; g_tex = g_tex_stack[0]; }
        else {
            int n = p[0] & 63; if (n & 32) n -= 64;
            g_pos_sp = (g_pos_sp - n) & 63;
            if (g_pos_sp >= 31) g_stack_err = 1;
            g_pos = g_pos_stack[g_pos_sp & 31]; g_vec = g_vec_stack[g_pos_sp & 31]; update_clip();
        }
        break; }
    case 0x13: /* STORE */
        if (g_mtx_mode == 0) g_proj_stack[0] = g_proj;
        else if (g_mtx_mode == 3) g_tex_stack[0] = g_tex;
        else { int i = p[0] & 31; if (i == 31) g_stack_err = 1; g_pos_stack[i] = g_pos; g_vec_stack[i] = g_vec; }
        break;
    case 0x14: /* RESTORE */
        if (g_mtx_mode == 0) { g_proj = g_proj_stack[0]; update_clip(); }
        else if (g_mtx_mode == 3) g_tex = g_tex_stack[0];
        else { int i = p[0] & 31; if (i == 31) g_stack_err = 1; g_pos = g_pos_stack[i]; g_vec = g_vec_stack[i]; update_clip(); }
        break;
    case 0x15: mtx_identity(&m); mtx_load(&m); break;
    case 0x16: memcpy(m.m, p, 64); mtx_load(&m); break;
    case 0x17: case 0x19:
        for (int i = 0; i < 4; i++) { for (int j = 0; j < 3; j++) m.m[i * 4 + j] = p[i * 3 + j]; m.m[i * 4 + 3] = i == 3 ? 1 << 12 : 0; }
        if (cmd == 0x17) mtx_load(&m); else mtx_apply(&m, 0);
        break;
    case 0x18: memcpy(m.m, p, 64); mtx_apply(&m, 0); break;
    case 0x1A:
        mtx_identity(&m);
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m.m[i * 4 + j] = p[i * 3 + j];
        mtx_apply(&m, 0);
        break;
    case 0x1B: mtx_identity(&m); m.m[0] = p[0]; m.m[5] = p[1]; m.m[10] = p[2]; mtx_apply(&m, 1); break;
    case 0x1C: mtx_identity(&m); m.m[12] = p[0]; m.m[13] = p[1]; m.m[14] = p[2]; mtx_apply(&m, 0); break;
    case 0x20: g_r = p[0] & 31; g_g = (p[0] >> 5) & 31; g_b = (p[0] >> 10) & 31; break;
    case 0x21: {
        int32_t nx = ((int32_t)(p[0] << 22) >> 22) << 3, ny = ((int32_t)(p[0] << 12) >> 22) << 3, nz = ((int32_t)(p[0] << 2) >> 22) << 3;
        if ((g_tex_param >> 30) == 2) {
            g_s = (int32_t)(((int64_t)nx * g_tex.m[0] + (int64_t)ny * g_tex.m[4] + (int64_t)nz * g_tex.m[8]) >> 24) + g_raw_s;
            g_t = (int32_t)(((int64_t)nx * g_tex.m[1] + (int64_t)ny * g_tex.m[5] + (int64_t)nz * g_tex.m[9]) >> 24) + g_raw_t;
        }
        do_lighting(nx, ny, nz);
        break; }
    case 0x22:
        g_raw_s = (int16_t)p[0]; g_raw_t = (int16_t)(p[0] >> 16);
        if ((g_tex_param >> 30) == 1) {
            g_s = (int32_t)(((int64_t)g_raw_s * g_tex.m[0] + (int64_t)g_raw_t * g_tex.m[4] + g_tex.m[8] + g_tex.m[12]) >> 12);
            g_t = (int32_t)(((int64_t)g_raw_s * g_tex.m[1] + (int64_t)g_raw_t * g_tex.m[5] + g_tex.m[9] + g_tex.m[13]) >> 12);
        } else { g_s = g_raw_s; g_t = g_raw_t; }
        break;
    case 0x23: g_vx = (int16_t)p[0]; g_vy = (int16_t)(p[0] >> 16); g_vz = (int16_t)p[1]; add_vertex(); break;
    case 0x24: g_vx = ((int32_t)(p[0] << 22) >> 22) << 6; g_vy = ((int32_t)(p[0] << 12) >> 22) << 6; g_vz = ((int32_t)(p[0] << 2) >> 22) << 6; add_vertex(); break;
    case 0x25: g_vx = (int16_t)p[0]; g_vy = (int16_t)(p[0] >> 16); add_vertex(); break;
    case 0x26: g_vx = (int16_t)p[0]; g_vz = (int16_t)(p[0] >> 16); add_vertex(); break;
    case 0x27: g_vy = (int16_t)p[0]; g_vz = (int16_t)(p[0] >> 16); add_vertex(); break;
    case 0x28:
        g_vx = (int16_t)(g_vx + (((int32_t)(p[0] << 22) >> 22) << 3)); g_vy = (int16_t)(g_vy + (((int32_t)(p[0] << 12) >> 22) << 3));
        g_vz = (int16_t)(g_vz + (((int32_t)(p[0] << 2) >> 22) << 3)); add_vertex(); break;
    case 0x29: g_poly_attr_pending = p[0]; break;
    case 0x2A: g_tex_param = p[0]; break;
    case 0x2B: g_pltt_base = p[0] & 0x1FFF; break;
    case 0x30: g_dif_amb = p[0]; if (p[0] & 0x8000) { g_r = p[0] & 31; g_g = (p[0] >> 5) & 31; g_b = (p[0] >> 10) & 31; } break;
    case 0x31: g_spe_emi = p[0]; break;
    case 0x32: {
        int l = (p[0] >> 30) & 3;
        int32_t lx = ((int32_t)(p[0] << 22) >> 22) << 3, ly = ((int32_t)(p[0] << 12) >> 22) << 3, lz = ((int32_t)(p[0] << 2) >> 22) << 3;
        for (int j = 0; j < 3; j++) g_light_vec[l][j] = (int32_t)(((int64_t)lx * g_vec.m[0 * 4 + j] + (int64_t)ly * g_vec.m[1 * 4 + j] + (int64_t)lz * g_vec.m[2 * 4 + j]) >> 12);
        break; }
    case 0x33: g_light_col[(p[0] >> 30) & 3] = p[0] & 0x7FFF; break;
    case 0x34: for (int i = 0; i < 32; i++) for (int k = 0; k < 4; k++) g_shininess[i * 4 + k] = (p[i] >> (k * 8)) & 0xFF; break;
    case 0x40: g_prim = p[0] & 3; g_prim_count = 0; g_strip_n = 0; g_poly_attr = g_poly_attr_pending; break;
    case 0x41: g_prim = -1; break;
    case 0x50: g_swap_pending = 1; g_swap_param = p[0]; break;
    case 0x60: g_viewport = p[0]; break;
    case 0x70: box_test(p); break;
    case 0x71: {
        g_vx = (int16_t)p[0]; g_vy = (int16_t)(p[0] >> 16); g_vz = (int16_t)p[1];
        int32_t in[4] = {g_vx, g_vy, g_vz, 1 << 12};
        for (int j = 0; j < 4; j++) { int64_t s = 0; for (int k = 0; k < 4; k++) s += (int64_t)in[k] * g_clip.m[k * 4 + j]; g_pos_result[j] = (int32_t)(s >> 12); }
        break; }
    case 0x72: {
        int32_t nx = ((int32_t)(p[0] << 22) >> 22) << 3, ny = ((int32_t)(p[0] << 12) >> 22) << 3, nz = ((int32_t)(p[0] << 2) >> 22) << 3;
        for (int j = 0; j < 3; j++) {
            int32_t v = (int32_t)(((int64_t)nx * g_vec.m[0 * 4 + j] + (int64_t)ny * g_vec.m[1 * 4 + j] + (int64_t)nz * g_vec.m[2 * 4 + j]) >> 12);
            if (v & 0x1000) v |= ~0xFFF; else v &= 0xFFF; /* 4.12 com sinal estendido a partir do bit 12 */
            g_vec_result[j] = (int16_t)v;
        }
        break; }
    }
}

/* fila do GXFIFO "empacotado": uma palavra com até 4 comandos, depois os parâmetros de cada um */
static uint8_t g_pk_cmd[4]; static int g_pk_n, g_pk_i, g_pk_got;
static int32_t g_params[32];
static void pk_next(void) {
    /* pula comandos sem parâmetros (executando-os) */
    while (g_pk_i < g_pk_n) {
        int c = g_pk_cmd[g_pk_i];
        if (!valid_cmd(c)) { g_pk_i++; continue; }
        if (g_nparams[c] == 0) { exec_cmd(c, g_params); g_pk_i++; continue; }
        break;
    }
    if (g_pk_i >= g_pk_n) g_pk_n = 0;
}
static void fifo_write(uint32_t v) {
    if (g_pk_n == 0) {
        g_pk_n = 0; g_pk_i = 0; g_pk_got = 0;
        for (int i = 0; i < 4; i++) g_pk_cmd[i] = (v >> (i * 8)) & 0xFF;
        /* comandos 0 no fim não contam */
        g_pk_n = 4; while (g_pk_n > 0 && g_pk_cmd[g_pk_n - 1] == 0) g_pk_n--;
        pk_next();
        return;
    }
    int c = g_pk_cmd[g_pk_i];
    g_params[g_pk_got++] = (int32_t)v;
    if (g_pk_got >= g_nparams[c]) { exec_cmd(c, g_params); g_pk_got = 0; g_pk_i++; pk_next(); }
}
/* portas diretas: cada endereço é um comando */
static int g_port_cmd = -1, g_port_got;
static void port_write(int cmd, uint32_t v) {
    if (!valid_cmd(cmd)) return;
    if (g_nparams[cmd] == 0) { exec_cmd(cmd, g_params); return; }
    if (g_port_cmd != cmd) { g_port_cmd = cmd; g_port_got = 0; }
    g_params[g_port_got++] = (int32_t)v;
    if (g_port_got >= g_nparams[cmd]) { exec_cmd(cmd, g_params); g_port_got = 0; g_port_cmd = -1; }
}

static uint32_t g_gxstat;
void gx_write32(uint32_t off, uint32_t v) {
    if (off >= 0x400 && off < 0x440) fifo_write(v);
    else if (off >= 0x440 && off < 0x600) port_write((off - 0x400) >> 2, v);
    else if (off == 0x600) { if (v & 0x8000) { g_stack_err = 0; } g_gxstat = (g_gxstat & ~0xC0000000u) | (v & 0xC0000000u); }
}
/* IRQ do GXFIFO (IF bit 21): por nível; como o FIFO daqui está sempre vazio, vale enquanto o modo != 0 */
int gx_fifo_irq(void) { return (g_gxstat >> 30) != 0; }
uint32_t gx_read32(uint32_t off) {
    if (off == 0x600) {
        uint32_t s = g_gxstat & 0xC0000000u;
        s |= (uint32_t)g_box_result << 1;
        s |= (uint32_t)(g_pos_sp & 31) << 8;
        s |= (uint32_t)(g_proj_sp & 1) << 13;
        s |= (uint32_t)g_stack_err << 15;
        s |= 3u << 25; /* FIFO vazio e com menos da metade; motor parado */
        return s;
    }
    if (off == 0x604) return (uint32_t)g_npolys[g_cur] | (0u << 16);
    if (off >= 0x620 && off < 0x630) return (uint32_t)g_pos_result[(off - 0x620) >> 2];
    if (off >= 0x630 && off < 0x638) { int i = (off - 0x630) >> 1; return (uint16_t)g_vec_result[i] | ((i + 1 < 3 ? (uint16_t)g_vec_result[i + 1] : 0) << 16); }
    if (off >= 0x640 && off < 0x680) return (uint32_t)g_clip.m[(off - 0x640) >> 2];
    if (off >= 0x680 && off < 0x6A4) { int i = (off - 0x680) >> 2; return (uint32_t)g_vec.m[(i / 3) * 4 + (i % 3)]; }
    return 0;
}

/* ---------- rasterização ---------- */
static uint32_t g_depth[256 * 192];
static uint8_t g_attr_id[256 * 192], g_fogflag[256 * 192], g_trans_id[256 * 192];

static uint16_t tex16(uint32_t a) { uint8_t *p = video_tex_ptr(a); return p ? (uint16_t)(p[0] | p[1] << 8) : 0; }
static uint8_t tex8(uint32_t a) { uint8_t *p = video_tex_ptr(a); return p ? *p : 0; }
static uint16_t pal16t(uint32_t a) { uint8_t *p = video_texpal_ptr(a); return p ? (uint16_t)(p[0] | p[1] << 8) : 0; }

/* cor RGB 5 bits + alfa 0..31; devolve 0 se transparente */
static int sample(const Poly *p, int s, int t, int *r, int *g, int *b, int *a) {
    uint32_t tp = p->tex;
    int fmt = (tp >> 26) & 7;
    int w = 8 << ((tp >> 20) & 7), h = 8 << ((tp >> 23) & 7);
    s >>= 4; t >>= 4;
    if (tp & (1 << 16)) { if (tp & (1 << 18)) { int q = s & w; s &= w - 1; if (q) s = w - 1 - s; } else s &= w - 1; }
    else s = s < 0 ? 0 : s >= w ? w - 1 : s;
    if (tp & (1 << 17)) { if (tp & (1 << 19)) { int q = t & h; t &= h - 1; if (q) t = h - 1 - t; } else t &= h - 1; }
    else t = t < 0 ? 0 : t >= h ? h - 1 : t;
    uint32_t base = (tp & 0xFFFF) << 3, pb = p->pltt << 4;
    uint32_t idx = (uint32_t)(t * w + s);
    uint16_t col; int alpha = 31;
    switch (fmt) {
    case 1: { uint8_t v = tex8(base + idx); col = pal16t(pb + (v & 31) * 2); alpha = ((v >> 5) * 4) + ((v >> 5) >> 1); break; }
    case 2: { uint8_t v = (tex8(base + idx / 4) >> ((idx & 3) * 2)) & 3; if (!v && (tp & (1 << 29))) return 0; col = pal16t((p->pltt << 3) + v * 2); break; }
    case 3: { uint8_t v = (tex8(base + idx / 2) >> ((idx & 1) * 4)) & 15; if (!v && (tp & (1 << 29))) return 0; col = pal16t(pb + v * 2); break; }
    case 4: { uint8_t v = tex8(base + idx); if (!v && (tp & (1 << 29))) return 0; col = pal16t(pb + v * 2); break; }
    case 5: { /* 4x4 comprimido */
        uint32_t blk = (uint32_t)((t >> 2) * (w >> 2) + (s >> 2));
        uint32_t word = base + blk * 4;
        uint8_t row = tex8(word + (t & 3));
        int v = (row >> ((s & 3) * 2)) & 3;
        uint32_t slot = word & 0x60000, ioff = (word & 0x1FFFF) >> 1;
        uint32_t ia = 0x20000 + (slot == 0x40000 ? 0x10000 : 0) + ioff;
        uint16_t info = tex16(ia);
        uint32_t pa = pb + (info & 0x3FFF) * 4;
        int mode = info >> 14;
        uint16_t c0 = pal16t(pa), c1 = pal16t(pa + 2);
        switch (v) {
        case 0: col = c0; break;
        case 1: col = c1; break;
        case 2:
            if (mode == 0 || mode == 2) col = pal16t(pa + 4);
            else if (mode == 1) col = (uint16_t)((((c0 & 31) + (c1 & 31)) >> 1) | (((((c0 >> 5) & 31) + ((c1 >> 5) & 31)) >> 1) << 5) | (((((c0 >> 10) & 31) + ((c1 >> 10) & 31)) >> 1) << 10));
            else col = (uint16_t)((((c0 & 31) * 5 + (c1 & 31) * 3) >> 3) | (((((c0 >> 5) & 31) * 5 + ((c1 >> 5) & 31) * 3) >> 3) << 5) | (((((c0 >> 10) & 31) * 5 + ((c1 >> 10) & 31) * 3) >> 3) << 10));
            break;
        default:
            if (mode == 0 || mode == 1) return 0;
            if (mode == 2) col = pal16t(pa + 6);
            else col = (uint16_t)((((c0 & 31) * 3 + (c1 & 31) * 5) >> 3) | (((((c0 >> 5) & 31) * 3 + ((c1 >> 5) & 31) * 5) >> 3) << 5) | (((((c0 >> 10) & 31) * 3 + ((c1 >> 10) & 31) * 5) >> 3) << 10));
            break;
        }
        break; }
    case 6: { uint8_t v = tex8(base + idx); col = pal16t(pb + (v & 7) * 2); alpha = v >> 3; break; }
    case 7: { uint16_t v = tex16(base + idx * 2); if (!(v & 0x8000)) return 0; col = v; break; }
    default: return 0;
    }
    if (alpha == 0) return 0;
    *r = col & 31; *g = (col >> 5) & 31; *b = (col >> 10) & 31; *a = alpha;
    return 1;
}

static const uint8_t *g_rio;
static uint16_t rio16(uint32_t off) { uint16_t v; memcpy(&v, g_rio + off, 2); return v; }

static void raster_poly(const Poly *p) {
    uint32_t disp = rio16(0x60);
    int mode = (p->attr >> 4) & 3, palpha = (p->attr >> 16) & 31, polyid = (p->attr >> 24) & 63;
    int wire = palpha == 0; if (wire) palpha = 31;
    int textured = ((disp & 1) && ((p->tex >> 26) & 7));
    int equal = (p->attr >> 14) & 1;
    int alpha_test = (disp >> 2) & 1, alpha_ref = rio16(0x340) & 31;
    int blend = (disp >> 3) & 1;
    for (int k = 1; k + 1 < p->n; k++) {
        const Vtx *a = &p->v[0], *b = &p->v[k], *c = &p->v[k + 1];
        float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
        if (fabsf(area) < 1e-6f) continue;
        int minx = (int)floorf(fminf(a->x, fminf(b->x, c->x))), maxx = (int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
        int miny = (int)floorf(fminf(a->y, fminf(b->y, c->y))), maxy = (int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
        if (minx < 0) minx = 0; if (miny < 0) miny = 0; if (maxx > 255) maxx = 255; if (maxy > 191) maxy = 191;
        float ia = 1.f / a->w, ib = 1.f / b->w, ic = 1.f / c->w;
        float inv_area = 1.f / area;
        for (int y = miny; y <= maxy; y++) {
            float py = y + 0.5f;
            for (int x = minx; x <= maxx; x++) {
                float px = x + 0.5f;
                float w0 = ((b->x - px) * (c->y - py) - (b->y - py) * (c->x - px)) * inv_area;
                float w1 = ((c->x - px) * (a->y - py) - (c->y - py) * (a->x - px)) * inv_area;
                float w2 = 1.f - w0 - w1;
                if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                if (wire && w0 > 0.08f && w1 > 0.08f && w2 > 0.08f) continue;
                int i = y * 256 + x;
                /* profundidade */
                float z = w0 * a->z + w1 * b->z + w2 * c->z;
                float q = w0 * ia + w1 * ib + w2 * ic; /* 1/w interpolado */
                uint32_t depth;
                if (p->wbuffer) { float wv = 1.f / q * 4096.f; depth = wv < 0 ? 0 : wv > 16777215.f ? 0xFFFFFF : (uint32_t)wv; }
                else { float zz = z * 16777215.f; depth = zz < 0 ? 0 : zz > 16777215.f ? 0xFFFFFF : (uint32_t)zz; }
                if (equal) { uint32_t d0 = g_depth[i]; if (depth > d0 + 0x200 || depth + 0x200 < d0) continue; }
                else if (depth >= g_depth[i]) continue;
                float pw = 1.f / q;
                float cr = (w0 * a->r * ia + w1 * b->r * ib + w2 * c->r * ic) * pw;
                float cg = (w0 * a->g * ia + w1 * b->g * ib + w2 * c->g * ic) * pw;
                float cb = (w0 * a->b * ia + w1 * b->b * ib + w2 * c->b * ic) * pw;
                int vr = (int)(cr + 0.5f), vg = (int)(cg + 0.5f), vb = (int)(cb + 0.5f);
                int r = vr, g = vg, bb = vb, al = palpha;
                if (textured) {
                    float s = (w0 * a->s * ia + w1 * b->s * ib + w2 * c->s * ic) * pw;
                    float t = (w0 * a->t * ia + w1 * b->t * ib + w2 * c->t * ic) * pw;
                    int tr, tg, tb, ta;
                    if (!sample(p, (int)floorf(s), (int)floorf(t), &tr, &tg, &tb, &ta)) continue;
                    if (mode == 1) { /* decalque */
                        r = (tr * ta + vr * (31 - ta)) / 31; g = (tg * ta + vg * (31 - ta)) / 31; bb = (tb * ta + vb * (31 - ta)) / 31;
                    } else if (mode == 2) {
                        uint16_t toon = rio16(0x380 + (vr & 31) * 2);
                        int or_ = toon & 31, og = (toon >> 5) & 31, ob = (toon >> 10) & 31;
                        if (disp & 2) { /* realce */
                            r = clamp31(((tr + 1) * (vr + 1) - 1) / 32 + or_); g = clamp31(((tg + 1) * (vr + 1) - 1) / 32 + og); bb = clamp31(((tb + 1) * (vr + 1) - 1) / 32 + ob);
                        } else { r = ((tr + 1) * (or_ + 1) - 1) / 32; g = ((tg + 1) * (og + 1) - 1) / 32; bb = ((tb + 1) * (ob + 1) - 1) / 32; }
                        al = ((ta + 1) * (palpha + 1) - 1) / 32;
                    } else { /* modulação */
                        r = ((tr + 1) * (vr + 1) - 1) / 32; g = ((tg + 1) * (vg + 1) - 1) / 32; bb = ((tb + 1) * (vb + 1) - 1) / 32;
                        al = ((ta + 1) * (palpha + 1) - 1) / 32;
                    }
                } else if (mode == 2) {
                    uint16_t toon = rio16(0x380 + (vr & 31) * 2);
                    if (disp & 2) { r = clamp31(vr + (toon & 31)); g = clamp31(vr + ((toon >> 5) & 31)); bb = clamp31(vr + ((toon >> 10) & 31)); }
                    else { r = toon & 31; g = (toon >> 5) & 31; bb = (toon >> 10) & 31; }
                }
                if (al <= 0) continue;
                if (alpha_test && al <= alpha_ref) continue;
                r = clamp31(r); g = clamp31(g); bb = clamp31(bb);
                if (al < 31 && blend) {
                    if (g_trans_id[i] == polyid + 1) continue; /* mesmo polígono translúcido não se sobrepõe */
                    g_trans_id[i] = (uint8_t)(polyid + 1);
                    int da = g_gx_alpha[i];
                    if (da) {
                        uint16_t d = g_gx_color[i];
                        r = (r * (al + 1) + (d & 31) * (31 - al)) / 32;
                        g = (g * (al + 1) + ((d >> 5) & 31) * (31 - al)) / 32;
                        bb = (bb * (al + 1) + ((d >> 10) & 31) * (31 - al)) / 32;
                        if (da > al) al = da;
                    }
                    if (p->attr & (1 << 11)) g_depth[i] = depth;
                } else {
                    g_depth[i] = depth;
                    g_attr_id[i] = (uint8_t)polyid;
                    g_trans_id[i] = 0;
                }
                g_gx_color[i] = (uint16_t)(r | g << 5 | bb << 10);
                g_gx_alpha[i] = (uint8_t)al;
                g_fogflag[i] = (p->attr >> 15) & 1;
            }
        }
    }
}

static int cmp_ymin(const void *x, const void *y) {
    const Poly *a = *(Poly *const *)x, *b = *(Poly *const *)y;
    return (a->ymin > b->ymin) - (a->ymin < b->ymin);
}
static void render(const uint8_t *io, int list, uint32_t swap) {
    g_rio = io;
    uint32_t cc; memcpy(&cc, io + 0x350, 4);
    uint16_t cd = rio16(0x354) & 0x7FFF;
    uint16_t ccol = cc & 0x7FFF; uint8_t calpha = (cc >> 16) & 31, cfog = (cc >> 15) & 1;
    uint32_t cdepth = ((uint32_t)cd << 9) | 0x1FF;
    for (int i = 0; i < 256 * 192; i++) {
        g_gx_color[i] = ccol; g_gx_alpha[i] = calpha; g_depth[i] = cdepth;
        g_attr_id[i] = (cc >> 24) & 63; g_fogflag[i] = cfog; g_trans_id[i] = 0;
    }
    int n = g_npolys[list];
    static Poly *order[MAX_POLYS];
    int k = 0;
    for (int i = 0; i < n; i++) { g_polys[list][i].wbuffer = (swap >> 1) & 1; if (!g_polys[list][i].translucent) order[k++] = &g_polys[list][i]; }
    int nop = k;
    for (int i = 0; i < n; i++) if (g_polys[list][i].translucent) order[k++] = &g_polys[list][i];
    qsort(order, nop, sizeof *order, cmp_ymin);
    if (!(swap & 1)) qsort(order + nop, k - nop, sizeof *order, cmp_ymin);
    for (int i = 0; i < k; i++) raster_poly(order[i]);
    /* neblina */
    uint16_t disp = rio16(0x60);
    if (disp & 0x80) {
        uint16_t fc = rio16(0x358), fa = (rio16(0x35A) & 31);
        uint16_t foff = rio16(0x35C) & 0x7FFF;
        int shift = (disp >> 8) & 15;
        int alpha_only = (disp >> 6) & 1;
        for (int i = 0; i < 256 * 192; i++) {
            if (!g_fogflag[i]) continue;
            int z = (int)(g_depth[i] >> 9);
            int step = 0x400 >> shift; if (step < 1) step = 1;
            int idx = (z - foff) / step;
            int dens;
            if (z < foff) dens = io[0x360] & 127;
            else if (idx >= 31) dens = io[0x360 + 31] & 127;
            else { int d0 = io[0x360 + idx] & 127, d1 = io[0x360 + idx + 1] & 127; int fr = (z - foff) - idx * step; dens = d0 + (d1 - d0) * fr / step; }
            if (dens >= 127) dens = 128;
            uint16_t c = g_gx_color[i];
            if (!alpha_only) {
                int r = ((fc & 31) * dens + (c & 31) * (128 - dens)) >> 7;
                int g = (((fc >> 5) & 31) * dens + ((c >> 5) & 31) * (128 - dens)) >> 7;
                int b = (((fc >> 10) & 31) * dens + ((c >> 10) & 31) * (128 - dens)) >> 7;
                g_gx_color[i] = (uint16_t)(r | g << 5 | b << 10);
            }
            g_gx_alpha[i] = (uint8_t)((fa * dens + g_gx_alpha[i] * (128 - dens)) >> 7);
        }
    }
}

void gx_vblank(const uint8_t *io) {
    if (!g_swap_pending) return;
    g_swap_pending = 0;
    int done = g_cur;
    g_cur ^= 1;
    g_npolys[g_cur] = 0;
    static int log = -1; static double acc; static int frames;
    if (log < 0) log = getenv("RECOMP_GXLOG") != NULL;
    struct timespec t0, t1;
    if (log) clock_gettime(CLOCK_MONOTONIC, &t0);
    render(io, done, g_swap_param);
    if (log) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        acc += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        if (++frames % 60 == 0) { fprintf(stderr, "[gx] %d polígonos, %.2f ms por quadro\n", g_npolys[done], acc / 60); acc = 0; }
    }
}

__attribute__((constructor)) static void gx_init(void) {
    mtx_identity(&g_proj); mtx_identity(&g_pos); mtx_identity(&g_vec); mtx_identity(&g_tex); mtx_identity(&g_clip);
}
