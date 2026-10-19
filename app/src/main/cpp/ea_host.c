/*
 * ea_host.c - runs the ORIGINAL Explode Arena (Symbian S60) engine code on a modern device.
 *
 * A small ARMv4 interpreter executes the unmodified game binary (ExplodeArena.app code image).
 * The handful of Symbian OS services the engine uses are provided by this file ("HLE"):
 *   - heap, TLS, file access (data.pak / game.cfg), CFbsBitmap data pointer, tick counter, key events.
 * Everything above those services (levels, sprites, AI, rendering, sound mixing) is the original code.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <setjmp.h>
#include "ea_host.h"
#include "slots.h"

typedef uint32_t u32; typedef int32_t s32; typedef uint8_t u8; typedef uint16_t u16; typedef uint64_t u64; typedef int64_t s64;

#define IMG_BASE   0x10000000u
#define IMG_SIZE   0x00052000u
#define HEAP_BASE  0x20000000u
#define HEAP_SIZE  0x00A00000u
#define STK_BASE   0x30000000u
#define STK_SIZE   0x00040000u
#define TRAP_BASE  0xF0000000u
#define HALT_ADDR  0xFFFF0000u
#define TRAPV_BASE 0xF1000000u
#define CODE_WORDS (IMG_SIZE / 4)

static u8 *img, *heap, *stk;
static u32 R[16];
static int fN, fZ, fC, fV;
static u64 icount;
static u64 icount_limit;
static jmp_buf jb;
static int jb_valid;
static char errbuf[256];
static int verbose_unhandled = 1;

static void fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(errbuf, sizeof errbuf, fmt, ap);
    va_end(ap);
    if (jb_valid) longjmp(jb, 1);
    fprintf(stderr, "FATAL: %s\n", errbuf);
    exit(9);
}
const char *ea_last_error(void) { return errbuf; }

/* ------------------------------------------------------------------ memory */
static inline u8 *mp(u32 a) {
    if (a - IMG_BASE < IMG_SIZE) return img + (a - IMG_BASE);
    if (a - HEAP_BASE < HEAP_SIZE) return heap + (a - HEAP_BASE);
    if (a - STK_BASE < STK_SIZE) return stk + (a - STK_BASE);
    fatal("bad memory access 0x%08x (pc=%08x lr=%08x)", a, R[15], R[14]);
    return NULL;
}
static inline u32 rd32(u32 a) { u32 v; memcpy(&v, mp(a & ~3u), 4); u32 r = (a & 3) * 8; return r ? (v >> r) | (v << (32 - r)) : v; }
static inline u32 rd16(u32 a) { u16 v; memcpy(&v, mp(a & ~1u), 2); return v; }
static inline u32 rd8(u32 a) { return *mp(a); }
static inline void wr32(u32 a, u32 v) { memcpy(mp(a & ~3u), &v, 4); }
static inline void wr16(u32 a, u32 v) { u16 w = (u16)v; memcpy(mp(a & ~1u), &w, 2); }
static inline void wr8(u32 a, u32 v) { *mp(a) = (u8)v; }

/* ------------------------------------------------------------------ allocator */
#define NB 262145u                      /* buckets: size/16 up to 4 MB */
static u32 *freehead;                   /* offsets into heap, 0 = empty */
static u32 heap_top;
static u32 halloc(u32 n) {
    u32 sz = (n + 15u) & ~15u; if (sz < 16) sz = 16;
    u32 b = sz >> 4, p = 0;
    if (b < NB && freehead[b]) {
        p = freehead[b];
        freehead[b] = *(u32 *)(heap + p - 8);
    } else {
        if (heap_top + sz + 16 > HEAP_SIZE) fatal("heap exhausted (request %u)", n);
        p = heap_top + 16; heap_top += sz + 16;
    }
    *(u32 *)(heap + p - 16) = sz; *(u32 *)(heap + p - 12) = 0xA110C8EDu;
    memset(heap + p, 0, sz);
    return HEAP_BASE + p;
}
static void hfree(u32 a) {
    if (!a) return;
    if (a - HEAP_BASE >= HEAP_SIZE) return;
    u32 p = a - HEAP_BASE;
    if (*(u32 *)(heap + p - 12) != 0xA110C8EDu) return;  /* not ours / double free */
    u32 sz = *(u32 *)(heap + p - 16), b = sz >> 4;
    *(u32 *)(heap + p - 12) = 0;
    if (b < NB) { *(u32 *)(heap + p - 8) = freehead[b]; freehead[b] = p; }
}

/* ------------------------------------------------------------------ virtual files */
static const u8 *pakhead; static u32 pakhead_len, pak_total;
#define MAX_ASSETS 128
static const u8 *asset_ptr[MAX_ASSETS]; static u32 asset_len[MAX_ASSETS]; static u8 asset_owned[MAX_ASSETS];
static u8 *pakhead_owned;
static u8 cfg_buf[4096]; static u32 cfg_len;

void ea_set_asset(int idx, const uint8_t *data, uint32_t len) {
    if (idx < 0 || idx >= MAX_ASSETS) return;
    if (asset_owned[idx]) { free((void *)asset_ptr[idx]); asset_owned[idx] = 0; }
    asset_ptr[idx] = data; asset_len[idx] = len;
}
void ea_set_asset_copy(int idx, const uint8_t *data, uint32_t len) {
    if (idx < 0 || idx >= MAX_ASSETS) return;
    u8 *p = malloc(len ? len : 1); if (!p) return;
    memcpy(p, data, len);
    if (asset_owned[idx]) free((void *)asset_ptr[idx]);
    asset_ptr[idx] = p; asset_len[idx] = len; asset_owned[idx] = 1;
}
const uint8_t *ea_config(uint32_t *len) { *len = cfg_len; return cfg_buf; }
void ea_set_config(const uint8_t *d, uint32_t len) { if (len > sizeof cfg_buf) len = sizeof cfg_buf; memcpy(cfg_buf, d, len); cfg_len = len; }
static int cfg_dirty;
int ea_config_dirty(void) { int d = cfg_dirty; cfg_dirty = 0; return d; }

static void cstr(u32 a, char *out, int max) { int i = 0; while (i < max - 1) { u8 c = rd8(a + i); if (!c) break; out[i++] = (char)c; } out[i] = 0; }

/* ------------------------------------------------------------------ CPU */
static inline int cond_ok(u32 c) {
    switch (c) {
    case 0: return fZ; case 1: return !fZ; case 2: return fC; case 3: return !fC;
    case 4: return fN; case 5: return !fN; case 6: return fV; case 7: return !fV;
    case 8: return fC && !fZ; case 9: return !fC || fZ;
    case 10: return fN == fV; case 11: return fN != fV;
    case 12: return !fZ && fN == fV; case 13: return fZ || fN != fV; case 14: return 1;
    }
    return 0;
}
static inline u32 ror(u32 v, u32 n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
static u32 shifter(u32 i, u32 pcval, int *cout) {
    u32 rm = i & 15, v = (rm == 15) ? pcval : R[rm];
    u32 type = (i >> 5) & 3, amt; int c = fC;
    if (i & 0x10) {
        u32 rs = (i >> 8) & 15; amt = R[rs] & 0xff;
        if (rm == 15) v += 4;
        if (amt == 0) { *cout = c; return v; }
        switch (type) {
        case 0: if (amt < 32) { c = (v >> (32 - amt)) & 1; v <<= amt; } else if (amt == 32) { c = v & 1; v = 0; } else { c = 0; v = 0; } break;
        case 1: if (amt < 32) { c = (v >> (amt - 1)) & 1; v >>= amt; } else if (amt == 32) { c = v >> 31; v = 0; } else { c = 0; v = 0; } break;
        case 2: if (amt < 32) { c = ((s32)v >> (amt - 1)) & 1; v = (u32)((s32)v >> amt); } else { c = v >> 31; v = (u32)((s32)v >> 31); } break;
        case 3: amt &= 31; if (amt == 0) { c = v >> 31; } else { c = (v >> (amt - 1)) & 1; v = ror(v, amt); } break;
        }
    } else {
        amt = (i >> 7) & 31;
        switch (type) {
        case 0: if (amt) { c = (v >> (32 - amt)) & 1; v <<= amt; } break;
        case 1: if (amt == 0) { c = v >> 31; v = 0; } else { c = (v >> (amt - 1)) & 1; v >>= amt; } break;
        case 2: if (amt == 0) { c = v >> 31; v = (u32)((s32)v >> 31); } else { c = ((s32)v >> (amt - 1)) & 1; v = (u32)((s32)v >> amt); } break;
        case 3: if (amt == 0) { u32 nc = v & 1; v = (v >> 1) | ((u32)fC << 31); c = nc; } else { c = (v >> (amt - 1)) & 1; v = ror(v, amt); } break;
        }
    }
    *cout = c; return v;
}
static inline void setNZ(u32 r) { fN = r >> 31; fZ = (r == 0); }
static inline u32 addc(u32 a, u32 b, u32 c, int s) {
    u64 r = (u64)a + b + c; u32 res = (u32)r;
    if (s) { setNZ(res); fC = (r >> 32) & 1; fV = (~(a ^ b) & (a ^ res)) >> 31; }
    return res;
}
static inline u32 subc(u32 a, u32 b, u32 c, int s) {
    u64 r = (u64)a + (u32)~b + c; u32 res = (u32)r;
    if (s) { setNZ(res); fC = (r >> 32) & 1; fV = ((a ^ b) & (a ^ res)) >> 31; }
    return res;
}
static inline void write_pc(u32 v) { R[15] = v & ~3u; }

#ifdef EA_PROFILE
u32 prof_tab[IMG_SIZE / 4];
void ea_prof_dump(const char *path) { FILE *f = fopen(path, "wb"); fwrite(prof_tab, 4, IMG_SIZE / 4, f); fclose(f); }
#endif
typedef void (*hook_fn)(void);
static hook_fn hooks[CODE_WORDS];
static void host_call(int slot);
static void audio_vcall(int slot);

static void step(void) {
    u32 pc = R[15];
    if (pc - IMG_BASE < IMG_SIZE) {
        hook_fn h = hooks[(pc - IMG_BASE) >> 2];
        if (h) { h(); R[15] = R[14] & ~1u; return; }
    } else if (pc - TRAP_BASE < 0x10000u) {
        host_call((int)((pc - TRAP_BASE) >> 2)); R[15] = R[14] & ~1u; return;
    } else if (pc - TRAPV_BASE < 0x1000u) {
        audio_vcall((int)((pc - TRAPV_BASE) >> 2)); R[15] = R[14] & ~1u; return;
    }
#ifdef EA_PROFILE
    { extern u32 prof_tab[]; if (pc - IMG_BASE < IMG_SIZE) prof_tab[(pc - IMG_BASE) >> 2]++; }
#endif
    u32 i = rd32(pc);
    R[15] = pc + 4;
    if (++icount > icount_limit) fatal("instruction limit exceeded at pc=%08x", pc);
    u32 cond = i >> 28;
    if (!cond_ok(cond)) return;
    u32 pcv = pc + 8;
    switch ((i >> 25) & 7) {
    case 0:
    case 1: {
        if ((i & 0x0ffffff0) == 0x012fff10) { u32 t = R[i & 15]; if (t & 1) fatal("thumb switch at %08x", pc); R[15] = t & ~3u; return; }
        if (((i >> 25) & 7) == 0 && (i & 0x90) == 0x90) {
            if ((i & 0x60) == 0) {
                if ((i & 0x0fc000f0) == 0x00000090) {
                    u32 rd = (i >> 16) & 15, rn = (i >> 12) & 15, rs = (i >> 8) & 15, rm = i & 15;
                    u32 r = R[rm] * R[rs]; if (i & 0x200000) r += R[rn];
                    R[rd] = r; if (i & 0x100000) setNZ(r); return;
                }
                if ((i & 0x0f8000f0) == 0x00800090) {
                    u32 rdhi = (i >> 16) & 15, rdlo = (i >> 12) & 15, rs = (i >> 8) & 15, rm = i & 15;
                    u64 r;
                    if (i & 0x400000) r = (u64)((s64)(s32)R[rm] * (s64)(s32)R[rs]); else r = (u64)R[rm] * (u64)R[rs];
                    if (i & 0x200000) r += ((u64)R[rdhi] << 32) | R[rdlo];
                    R[rdlo] = (u32)r; R[rdhi] = (u32)(r >> 32);
                    if (i & 0x100000) { fN = (r >> 63) & 1; fZ = (r == 0); }
                    return;
                }
                if ((i & 0x0fb000f0) == 0x01000090) {
                    u32 rn = (i >> 16) & 15, rd = (i >> 12) & 15, rm = i & 15; u32 a = R[rn];
                    if (i & 0x400000) { u32 t = rd8(a); wr8(a, R[rm]); R[rd] = t; } else { u32 t = rd32(a); wr32(a, R[rm]); R[rd] = t; }
                    return;
                }
            } else {
                u32 L = i & 0x100000, P = i & 0x1000000, U = i & 0x800000, W = i & 0x200000;
                u32 rn = (i >> 16) & 15, rd = (i >> 12) & 15, sh = (i >> 5) & 3;
                u32 off = (i & 0x400000) ? (((i >> 4) & 0xf0) | (i & 15)) : R[i & 15];
                u32 base = (rn == 15) ? pcv : R[rn];
                u32 addr = P ? (U ? base + off : base - off) : base;
                if (L) {
                    u32 v;
                    if (sh == 1) v = rd16(addr); else if (sh == 2) v = (u32)(s32)(int8_t)rd8(addr); else v = (u32)(s32)(int16_t)rd16(addr);
                    if ((!P) || W) R[rn] = P ? addr : (U ? base + off : base - off);
                    if (rd == 15) write_pc(v); else R[rd] = v;
                } else {
                    if (sh != 1) fatal("ldrd/strd unsupported at %08x", pc);
                    wr16(addr, R[rd]);
                    if ((!P) || W) R[rn] = P ? addr : (U ? base + off : base - off);
                }
                return;
            }
        }
        u32 opc = (i >> 21) & 15, S = i & 0x100000;
        if (opc >= 8 && opc <= 11 && !S) fatal("PSR/misc op %08x at %08x unsupported", i, pc);
        u32 rn = (i >> 16) & 15, rd = (i >> 12) & 15;
        u32 a = (rn == 15) ? ((!(i & 0x2000000) && (i & 0x10)) ? pcv + 4 : pcv) : R[rn];
        u32 b; int sc = fC;
        if (i & 0x2000000) { u32 imm = i & 0xff, rot = ((i >> 8) & 15) * 2; b = ror(imm, rot); if (rot) sc = b >> 31; }
        else b = shifter(i, pcv, &sc);
        u32 res = 0; int wb = 1, logical = 0;
        switch (opc) {
        case 0: res = a & b; logical = 1; break;
        case 1: res = a ^ b; logical = 1; break;
        case 2: res = subc(a, b, 1, S && rd != 15); break;
        case 3: res = subc(b, a, 1, S && rd != 15); break;
        case 4: res = addc(a, b, 0, S && rd != 15); break;
        case 5: res = addc(a, b, fC, S && rd != 15); break;
        case 6: res = subc(a, b, fC, S && rd != 15); break;
        case 7: res = subc(b, a, fC, S && rd != 15); break;
        case 8: res = a & b; logical = 1; wb = 0; break;
        case 9: res = a ^ b; logical = 1; wb = 0; break;
        case 10: subc(a, b, 1, 1); wb = 0; break;
        case 11: addc(a, b, 0, 1); wb = 0; break;
        case 12: res = a | b; logical = 1; break;
        case 13: res = b; logical = 1; break;
        case 14: res = a & ~b; logical = 1; break;
        case 15: res = ~b; logical = 1; break;
        }
        if (logical && S && rd != 15) { setNZ(res); fC = sc; }
        if (wb) { if (rd == 15) write_pc(res); else R[rd] = res; }
        return;
    }
    case 2:
    case 3: {
        if (((i >> 25) & 7) == 3 && (i & 0x10)) fatal("undefined %08x at %08x", i, pc);
        u32 L = i & 0x100000, B = i & 0x400000, P = i & 0x1000000, U = i & 0x800000, W = i & 0x200000;
        u32 rn = (i >> 16) & 15, rd = (i >> 12) & 15, off;
        if (!(i & 0x2000000)) off = i & 0xfff; else { int c; off = shifter(i & ~0x10u, pcv, &c); }
        u32 base = (rn == 15) ? pcv : R[rn];
        u32 addr = P ? (U ? base + off : base - off) : base;
        if (L) {
            u32 v = B ? rd8(addr) : rd32(addr);
            if (!P || W) R[rn] = P ? addr : (U ? base + off : base - off);
            if (rd == 15) write_pc(v); else R[rd] = v;
        } else {
            u32 v = (rd == 15) ? pcv + 4 : R[rd];
            if (B) wr8(addr, v); else wr32(addr, v);
            if (!P || W) R[rn] = P ? addr : (U ? base + off : base - off);
        }
        return;
    }
    case 4: {
        u32 L = i & 0x100000, P = i & 0x1000000, U = i & 0x800000, W = i & 0x200000;
        u32 rn = (i >> 16) & 15, list = i & 0xffff;
        int n = __builtin_popcount(list);
        u32 base = R[rn], start;
        if (U) start = P ? base + 4 : base; else start = P ? base - 4 * n : base - 4 * n + 4;
        u32 newbase = U ? base + 4 * n : base - 4 * n;
        u32 a = start;
        if (L) {
            if (W) R[rn] = newbase;
            for (int r = 0; r < 16; r++) if (list & (1u << r)) { u32 v = rd32(a); a += 4; if (r == 15) write_pc(v); else R[r] = v; }
        } else {
            for (int r = 0; r < 16; r++) if (list & (1u << r)) { wr32(a, r == 15 ? pcv + 4 : R[r]); a += 4; }
            if (W) R[rn] = newbase;
        }
        return;
    }
    case 5: {
        s32 off = (s32)(i << 8) >> 6;
        if (i & 0x1000000) R[14] = pc + 4;
        R[15] = pcv + off;
        return;
    }
    default:
        fatal("unsupported instruction %08x at %08x", i, pc);
    }
}

/* call a guest function; args in r0-r3, extra args on the stack */
static u32 call(u32 off, u32 a0, u32 a1, u32 a2, u32 a3, const u32 *sargs, int ns) {
    u32 saved[16]; memcpy(saved, R, sizeof R);
    R[0] = a0; R[1] = a1; R[2] = a2; R[3] = a3;
    u32 sp = (saved[13] >= STK_BASE && saved[13] < STK_BASE + STK_SIZE && saved[13] != 0) ? saved[13] - 64 : STK_BASE + STK_SIZE - 0x100;
    sp &= ~7u;
    sp -= 4 * ns; for (int k = 0; k < ns; k++) wr32(sp + 4 * k, sargs[k]);
    R[13] = sp; R[14] = HALT_ADDR; R[15] = IMG_BASE + off;
    while (R[15] != HALT_ADDR) step();
    u32 r = R[0];
    memcpy(R, saved, sizeof R);
    return r;
}

/* ------------------------------------------------------------------ hooks (guest function replacements) */
static u32 tls_value;
static void h_settls(void) { tls_value = R[0]; }
static void h_gettls(void) { R[0] = tls_value; }
static void h_alloc2(void) { R[0] = halloc(R[1]); }          /* Allocator_Alloc(this, size) */
static void h_free2(void) { hfree(R[1]); }                   /* Allocator_Free(this, ptr)   */
static void h_alloc1(void) { R[0] = halloc(R[0]); }          /* GameMalloc(size)            */
static void h_free1(void) { hfree(R[0]); }                   /* GameFree(ptr)               */


/* ------------------------------------------------------------------ native versions of the engine's hottest tile blitters
   (8x8 tiles, 4 bits/pixel, 16-entry RGB565 palette, destination stride 384 bytes). Behaviour is identical to the ARM code
   they replace; EA_NOBLIT builds the pure-emulation reference used for A/B verification. */
static inline u16 rdh(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }
static inline void wrh(u8 *p, u16 v) { memcpy(p, &v, 2); }
static void h_tile_opaque(void) {            /* sub_028468 */
    u8 *d = mp(R[0]); const u8 *s = mp(R[1]), *pal = mp(R[2]);
    for (int y = 0; y < 8; y++, d += 0x180, s += 4) {
        u32 w; memcpy(&w, s, 4);
        for (int x = 0; x < 8; x++) wrh(d + 2 * x, rdh(pal + 2 * ((w >> (4 * x)) & 15)));
    }
}
static void h_tile_trans(void) {             /* sub_028934: nibble 0 = transparent */
    u8 *d = mp(R[0]); const u8 *s = mp(R[1]), *pal = mp(R[2]);
    for (int y = 0; y < 8; y++, d += 0x180, s += 4) {
        u32 w; memcpy(&w, s, 4);
        for (int x = 0; x < 8; x++) { u32 n = (w >> (4 * x)) & 15; if (n) wrh(d + 2 * x, rdh(pal + 2 * n)); }
    }
}
static inline u16 blend_add(u16 dv, u16 pv) {
    u32 r = (pv & 31) + (dv & 31), g = ((pv >> 6) & 31) + ((dv >> 6) & 31), b = ((pv >> 11) & 31) + ((dv >> 11) & 31);
    if (r >= 31) { r = 31; }
    if (g >= 31) { g = 31; }
    if (b >= 31) { b = 31; }
    return (u16)(r | (g << 6) | (b << 11));
}
static inline u16 blend_sub(u16 dv, u16 pv) {
    int r = (int)(dv & 31) - (int)(pv & 31), g = (int)((dv >> 6) & 31) - (int)((pv >> 6) & 31), b = (int)((dv >> 11) & 31) - (int)((pv >> 11) & 31);
    if (r < 0) { r = 0; }
    if (g < 0) { g = 0; }
    if (b < 0) { b = 0; }
    return (u16)(r | (g << 6) | (b << 11));
}
static void tile_blend(int sub, int skip0) {
    u8 *d = mp(R[0]); const u8 *s = mp(R[1]), *pal = mp(R[2]);
    for (int y = 0; y < 8; y++, d += 0x180) {
        for (int k = 0; k < 4; k++) {
            u8 b = *s++;
            for (int h = 0; h < 2; h++) {
                u32 n = h ? (b >> 4) : (b & 15);
                u8 *dp = d + 2 * (2 * k + h);
                if (skip0 && !n) continue;
                u16 pv = rdh(pal + 2 * n), dv = rdh(dp);
                wrh(dp, sub ? blend_sub(dv, pv) : blend_add(dv, pv));
            }
        }
    }
}
static void h_fxmul(void) { R[0] = (u32)(((s64)(s32)R[0] * (s64)(s32)R[1]) >> 16); }   /* sub_003534: 16.16 fixed-point multiply */
static void h_tile_add(void) { tile_blend(0, 0); }          /* sub_028784 */
static void h_tile_add_t(void) { tile_blend(0, 1); }        /* sub_028c80 */
static void h_tile_sub_t(void) { tile_blend(1, 1); }        /* sub_028d74 */

static const u8 *vfile(const char *name, u32 *len) {
    if (!strcmp(name, "data.pak")) { *len = pak_total; return pakhead; }
    if (!strcmp(name, "game.cfg")) { *len = cfg_len; return cfg_len ? cfg_buf : NULL; }
    *len = 0; return NULL;
}
static void h_filesize(void) {                              /* sub_00a888(file, name) -> size or 0 */
    char nm[64]; cstr(R[1], nm, sizeof nm);
    u32 len; vfile(nm, &len);
    R[0] = len;
}
static void h_readat(void) {                                /* Pak_ReadAt(file, name, dst, off, size) */
    u32 file = R[0], name = R[1], dst = R[2], off = R[3], size = rd32(R[13]);
    char nm[64]; cstr(name, nm, sizeof nm);
    u8 key = (u8)rd8(file);
    u32 len; const u8 *src = vfile(nm, &len);
    u32 avail = !strcmp(nm, "data.pak") ? pakhead_len : len;
    for (u32 k = 0; k < size; k++) {
        u8 b = (src && off + k < avail) ? src[off + k] : 0;
        wr8(dst + k, b ^ key);
    }
    R[0] = 0;
}
static void h_nop(void) {}
/* sub_01226c(out1, out2, out3): formats the device IMEI into three small ID strings.
   Re-implemented with a fixed dummy IMEI so no real device identifier is ever read. */
static void h_idfmt(void) {
    u32 o1 = R[0], o2 = R[1], o3 = R[2];
    const char *imei = "351234567890123";
    char f[32]; int n = 18;
    memcpy(f, imei, 6); f[6] = '-'; f[7] = imei[6]; f[8] = imei[7]; f[9] = '-';
    memcpy(f + 10, imei + 8, 6); f[16] = '-'; f[17] = imei[14]; f[18] = 0;
    for (int k = 0; k < 19; k++) wr8(o1 + k, (u8)f[k]);
    for (int k = 0; k < 5; k++) wr8(o2 + k, (u8)f[k]);
    for (int k = 0; k < 5; k++) wr8(o2 + 9 - k, (u8)f[n - 1 - k]);
    int c = 0;
    for (int i = 0; c < 5 && i < n; i++) if (f[i] >= '0' && f[i] <= '9') wr8(o3 + c++, (u8)f[i]);
    c = 0;
    for (int i = n - 1; c < 5 && i >= 0; i--) if (f[i] >= '0' && f[i] <= '9') { wr8(o3 + 9 - c, (u8)f[i]); c++; }
}
static void h_writefile(void) {                             /* sub_00ab18(file, name, src, size) -> 1 */
    u32 file = R[0], name = R[1], src = R[2], size = R[3];
    char nm[64]; cstr(name, nm, sizeof nm);
    u8 key = (u8)rd8(file);
    if (!strcmp(nm, "game.cfg") && size <= sizeof cfg_buf) {
        for (u32 k = 0; k < size; k++) cfg_buf[k] = (u8)(rd8(src + k) ^ key);
        cfg_len = size; cfg_dirty = 1;
    }
    R[0] = 1;
}
static void h_readindex(void) {                             /* Pak_ReadByIndex(pak, idx, dst, size) */
    u32 idx = R[1], dst = R[2];
    if (idx >= MAX_ASSETS || !asset_ptr[idx]) fatal("asset %u missing", idx);
    memcpy(mp(dst), asset_ptr[idx], asset_len[idx]);
}

/* ------------------------------------------------------------------ C library helpers */
static u32 va_ptr;                              /* when non-zero: args come from a va_list in guest memory */
static u32 va_next(int *n) {                    /* printf-style args: r2, r3, then stack (or va_list) */
    u32 v;
    if (va_ptr) v = rd32(va_ptr + 4 * (u32)*n);
    else if (*n == 0) v = R[2]; else if (*n == 1) v = R[3]; else v = rd32(R[13] + 4 * (u32)(*n - 2));
    (*n)++; return v;
}
static u32 do_sprintf(u32 dst, u32 fmt) {
    int an = 0; u32 o = 0;
    for (u32 i = 0;; i++) {
        u8 c = rd8(fmt + i);
        if (c != '%') { wr8(dst + o++, c); if (!c) return o - 1; continue; }
        int zero = 0, width = 0, lng = 0, left = 0;
        c = rd8(fmt + ++i);
        for (;; c = rd8(fmt + ++i)) { if (c == '0') zero = 1; else if (c == '-') left = 1; else break; }
        while (c >= '0' && c <= '9') { width = width * 10 + (c - '0'); c = rd8(fmt + ++i); }
        while (c == 'l' || c == 'h') { lng = 1; c = rd8(fmt + ++i); }
        char tmp[40]; int tl = 0;
        if (c == '%') { wr8(dst + o++, '%'); continue; }
        if (c == 's') {
            u32 p = va_next(&an); u32 k = 0;
            while (rd8(p + k)) tmp[0] = 0, wr8(dst + o++, rd8(p + k)), k++;
            continue;
        }
        u32 v = va_next(&an); (void)lng;
        if (c == 'c') { wr8(dst + o++, v); continue; }
        if (c == 'd' || c == 'i') { snprintf(tmp, sizeof tmp, "%d", (int)v); }
        else if (c == 'u') snprintf(tmp, sizeof tmp, "%u", v);
        else if (c == 'x') snprintf(tmp, sizeof tmp, "%x", v);
        else if (c == 'X') snprintf(tmp, sizeof tmp, "%X", v);
        else { tmp[0] = '?'; tmp[1] = 0; }
        tl = (int)strlen(tmp);
        int pad = width > tl ? width - tl : 0;
        if (!left) { for (int k = 0; k < pad; k++) wr8(dst + o++, zero ? '0' : ' '); }
        for (int k = 0; k < tl; k++) wr8(dst + o++, (u8)tmp[k]);
        if (left) { for (int k = 0; k < pad; k++) wr8(dst + o++, ' '); }
    }
}


/* ------------------------------------------------------------------ audio (CMdaAudioOutputStream HLE) */
#include <pthread.h>
static u32 au_cb, au_stream, au_vt;
static int au_open_pending, au_write_done, au_state;
static u32 au_last_desc, au_rate_enum = 4, au_chan_enum = 0, au_vol = 0;
static int au_rate_hz = 16000;      /* the engine's mixer is hard-wired to 16 kHz mono 16-bit */
#define RING_N (1 << 16)
static int16_t ring[RING_N]; static volatile u32 ring_r, ring_w;
static pthread_mutex_t ring_mx = PTHREAD_MUTEX_INITIALIZER;
static u32 ring_fill(void) { return ring_w - ring_r; }
static int au_log;
static int rate_from_enum(u32 e) __attribute__((unused));
static int rate_from_enum(u32 e) {
    static const int rates[] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000 };
    for (int b = 0; b < 9; b++) if (e & (1u << b)) return rates[b];
    return 16000;
}
static void au_push(u32 desc) {
    u32 hd = rd32(desc), type = hd >> 28, len = hd & 0x0fffffffu, p;
    switch (type) { case 0: p = desc + 4; break; case 1: p = rd32(desc + 4); break; case 2: p = rd32(desc + 8); break; case 3: p = desc + 8; break; default: p = rd32(desc + 4) + 4; break; }
    u32 ns = len / 2;
    pthread_mutex_lock(&ring_mx);
    for (u32 k = 0; k < ns; k++) {
        if (ring_fill() >= RING_N) break;
        ring[ring_w & (RING_N - 1)] = (int16_t)rd16(p + 2 * k); ring_w++;
    }
    pthread_mutex_unlock(&ring_mx);
    if (au_log) fprintf(stderr, "[audio] WriteL %u bytes (%u samples) rate=%d fill=%u\n", len, ns, au_rate_hz, ring_fill());
}
static void audio_vcall(int slot) {
    u32 a1 = R[1], a2 = R[2];
    switch (slot) {
    case 2: R[0] = 0; au_state = 0; return;                                      /* ~CMdaAudioOutputStream */
    case 3: au_rate_enum = a1; au_chan_enum = a2;
            if (au_log) fprintf(stderr, "[audio] SetAudioPropertiesL rate=%08x chan=%08x\n", a1, a2);
            R[0] = 0; return;
    case 4: au_open_pending = 1; au_state = 1; R[0] = 0; return;                 /* Open */
    case 5: R[0] = 100; return;                                                  /* MaxVolume */
    case 6: R[0] = au_vol; return;                                               /* Volume */
    case 7: au_vol = a1; R[0] = 0; return;                                       /* SetVolume */
    case 8: R[0] = 0; return;                                                    /* SetPriority */
    case 9: au_last_desc = a1; au_push(a1); au_write_done = 1; R[0] = 0; return; /* WriteL */
    case 10: au_state = 0; au_write_done = 0; R[0] = 0; return;                  /* Stop */
    default: if (au_log) fprintf(stderr, "[audio] unhandled vcall slot %d\n", slot); R[0] = 0; return;
    }
}
static u32 call_cb(int slot, u32 a1, u32 a2);
static void audio_service(void) {
    if (!au_cb) return;
    if (au_open_pending) { au_open_pending = 0; au_state = 2; call_cb(0, 0, 0); }              /* MaoscOpenComplete(KErrNone) */
    for (int k = 0; k < 3 && au_write_done && ring_fill() < (u32)(au_rate_hz / 8); k++) { au_write_done = 0; call_cb(1, 0, au_last_desc); } /* MaoscBufferCopied */
}
int ea_audio_read(int16_t *out, int n) {                                         /* audio thread */
    pthread_mutex_lock(&ring_mx);
    int got = 0;
    while (got < n && ring_r != ring_w) { out[got++] = ring[ring_r & (RING_N - 1)]; ring_r++; }
    pthread_mutex_unlock(&ring_mx);
    while (got < n) out[got++] = 0;
    return au_rate_hz;
}
int ea_audio_rate(void) { return au_rate_hz; }
unsigned ea_audio_fill(void) { return ring_fill(); }
int ea_audio_state(void) { return au_state * 100 + au_write_done * 10 + au_open_pending; }
void ea_audio_log(int on) { au_log = on; }

static u32 guest_cmp(u32 fn, u32 a, u32 b) { return call(fn - IMG_BASE, a, b, 0, 0, NULL, 0); }
static void do_qsort(u32 base, u32 n, u32 sz, u32 cmp) {          /* stable merge sort, comparator runs in the guest */
    if (n < 2 || sz == 0 || sz > 256) return;
    u8 *tmp = malloc((size_t)n * sz), *a = malloc((size_t)n * sz);
    if (!tmp || !a) { free(tmp); free(a); return; }
    memcpy(a, mp(base), (size_t)n * sz);
    u32 scratch = halloc(2 * sz);
    for (u32 w = 1; w < n; w <<= 1) {
        for (u32 i = 0; i < n; i += 2 * w) {
            u32 m = i + w < n ? i + w : n, e = i + 2 * w < n ? i + 2 * w : n, l = i, r = m, k = i;
            while (l < m && r < e) {
                memcpy(mp(scratch), a + (size_t)l * sz, sz); memcpy(mp(scratch + sz), a + (size_t)r * sz, sz);
                int c = (int)guest_cmp(cmp, scratch, scratch + sz);
                if (c <= 0) { memcpy(tmp + (size_t)k * sz, a + (size_t)l * sz, sz); l++; } else { memcpy(tmp + (size_t)k * sz, a + (size_t)r * sz, sz); r++; }
                k++;
            }
            while (l < m) { memcpy(tmp + (size_t)k * sz, a + (size_t)l * sz, sz); l++; k++; }
            while (r < e) { memcpy(tmp + (size_t)k * sz, a + (size_t)r * sz, sz); r++; k++; }
        }
        u8 *t = a; a = tmp; tmp = t;
    }
    memcpy(mp(base), a, (size_t)n * sz);
    hfree(scratch); free(a); free(tmp);
}

/* ------------------------------------------------------------------ Symbian import HLE */
static u32 tickbase; static u32 ticks;                       /* 1 tick = 1/64 s (EKA1) */
void ea_set_ticks(uint32_t t) { ticks = t; }
static u32 imei_dummy;

static void host_call(int slot) {
    if (slot < 0 || slot >= EA_NSLOTS) fatal("bad import slot %d", slot);
    int dll = EA_SLOT_DLL[slot], ord = EA_SLOT_ORD[slot];
    u32 a0 = R[0], a1 = R[1], a2 = R[2];
    switch ((dll << 16) | ord) {
    case (DLL_EUSER << 16) | 1505: case (DLL_EUSER << 16) | 3: case (DLL_EUSER << 16) | 35:
    case (DLL_EUSER << 16) | 1582: case (DLL_EUSER << 16) | 45: case (DLL_EUSER << 16) | 1507:
        R[0] = halloc(a0); return;                                         /* operator new / User::Alloc */
    case (DLL_EUSER << 16) | 1504: case (DLL_EUSER << 16) | 476:
        hfree(a0); R[0] = 0; return;                                       /* operator delete / User::Free */
    case (DLL_EUSER << 16) | 2:                                            /* Mem::Copy */
        for (u32 i = 0; i < a2; i++) wr8(a0 + i, rd8(a1 + i));
        R[0] = a0 + a2; return;
    case (DLL_EUSER << 16) | 416:                                          /* Mem::Fill(dst, len, ch) */
        for (u32 i = 0; i < a1; i++) wr8(a0 + i, a2);
        R[0] = a0 + a1; return;
    case (DLL_EUSER << 16) | 1367:                                          /* TPtr8::TPtr8(buf, len, maxlen) */
        wr32(a0, a2 | (2u << 28)); wr32(a0 + 4, R[3]); wr32(a0 + 8, a1);
        R[0] = a0; return;
    case (DLL_SYSAGT << 16) | 18: case (DLL_SYSAGT << 16) | 20: case (DLL_SYSAGT << 16) | 2:
    case (DLL_EUSER << 16) | 172:                                           /* RSystemAgent connect/get-state/close: not silent */
        R[0] = 0; return;
    case (DLL_EUSER << 16) | 411:                                           /* Mem::FillZ(ptr, len) */
        for (u32 i = 0; i < a1; i++) wr8(a0 + i, 0);
        R[0] = a0; return;
    case (DLL_EUSER << 16) | 243:                                           /* memcpy-like copy(dst, src, len) */
        for (u32 i = 0; i < a2; i++) wr8(a0 + i, rd8(a1 + i));
        R[0] = a0; return;
    case (DLL_EUSER << 16) | 1359: R[0] = a0; return;                      /* CBase::CBase() */
    case (DLL_EUSER << 16) | 1137: R[0] = ticks; return;                   /* User::TickCount */
    case (DLL_EUSER << 16) | 21: R[0] = 0; return;                         /* User::After */
    case (DLL_EUSER << 16) | 1514: { s32 n = (s32)a0, d = (s32)a1; R[0] = d ? (u32)(n / d) : 0; R[1] = d ? (u32)(n % d) : 0; return; }
    case (DLL_EUSER << 16) | 1529: { float f = (float)(s32)a0; memcpy(&R[0], &f, 4); return; }                 /* __floatsisf */
    case (DLL_EUSER << 16) | 1455: { float x, y, r; memcpy(&x, &R[0], 4); memcpy(&y, &R[1], 4); r = x + y; memcpy(&R[0], &r, 4); return; } /* __addsf3 */
    case (DLL_EUSER << 16) | 1513: { float x, y, r; memcpy(&x, &R[0], 4); memcpy(&y, &R[1], 4); r = y != 0.0f ? x / y : 0.0f; memcpy(&R[0], &r, 4); return; } /* __divsf3 */
    case (DLL_EUSER << 16) | 1393: { float f; memcpy(&f, &R[1], 4); double d = (double)f; u8 *p = mp(a0); memcpy(p, &d, 8); R[0] = a0; return; }  /* TRealX::TRealX(TReal32) */
    case (DLL_EUSER << 16) | 1392: { double d = (double)(s64)(((u64)a2 << 32) | a1); u8 *p = mp(a0); memcpy(p, &d, 8); R[0] = a0; return; }   /* TRealX::TRealX(TInt64) */
    case (DLL_EUSER << 16) | 1587: { double d; memcpy(&d, mp(a0), 8); R[0] = (u32)(s32)d; return; }                                          /* TRealX -> TInt */
    case (DLL_MEDIACLIENTAUDIOSTREAM << 16) | 1: {                           /* CMdaAudioOutputStream::NewL(callback) */
        au_cb = a0;
        if (!au_stream) {
            au_stream = halloc(16); au_vt = halloc(16 * 4);
            for (int k = 2; k < 16; k++) wr32(au_vt + 4 * k, TRAPV_BASE + 4 * k);
            wr32(au_stream, au_vt);
        }
        R[0] = au_stream; return; }
    case (DLL_EUSER << 16) | 1617: R[0] = a1 ? a0 % a1 : 0; return;               /* __umodsi3 */
    case (DLL_EUSER << 16) | 1616: { u32 d = a1; R[0] = d ? a0 / d : 0; R[1] = d ? a0 % d : 0; return; }
    case (DLL_FBSCLI << 16) | 135: wr32(a0, 0); R[0] = a0; return;          /* CFbsBitmap::CFbsBitmap() */
    case (DLL_FBSCLI << 16) | 16: {                                        /* CFbsBitmap::Create(TSize&, TDisplayMode) */
        u32 w = rd32(a1), h = rd32(a1 + 4), bytes = w * h * 2;
        u32 d = halloc(bytes);
        wr32(a0, d); wr32(a0 + 4, w); wr32(a0 + 8, h); wr32(a0 + 12, a2);
        R[0] = 0; return; }
    case (DLL_FBSCLI << 16) | 17: R[0] = rd32(a0); return;                 /* CFbsBitmap::DataAddress() */
    case (DLL_ESTLIB << 16) | 82: va_ptr = 0; R[0] = do_sprintf(a0, a1); return;       /* sprintf */
    case (DLL_ESTLIB << 16) | 90: va_ptr = a2; R[0] = do_sprintf(a0, a1); va_ptr = 0; return;   /* vsprintf */
    case (DLL_ESTLIB << 16) | 133: do_qsort(a0, a1, a2, R[3]); R[0] = 0; return;        /* qsort(base, n, size, cmp) */
    case (DLL_ESTLIB << 16) | 7: { u32 n = a2; if (a0 <= a1) for (u32 i = 0; i < n; i++) wr8(a0 + i, rd8(a1 + i)); else for (u32 i = n; i > 0; i--) wr8(a0 + i - 1, rd8(a1 + i - 1)); R[0] = a0; return; } /* memmove */
    case (DLL_ESTLIB << 16) | 9: { u32 n = 0; while (rd8(a0 + n)) n++; R[0] = n; return; }                /* strlen */
    case (DLL_ESTLIB << 16) | 173: { u32 r = 0; for (u32 i = 0; i < a2; i++) if (rd8(a0 + i) == (a1 & 0xff)) { r = a0 + i; break; } R[0] = r; return; } /* memchr */
    case (DLL_ESTLIB << 16) | 178: { u32 d = 0; while (rd8(a0 + d)) d++; u32 i = 0; for (;;) { u8 c = rd8(a1 + i); wr8(a0 + d + i, c); if (!c) break; i++; } R[0] = a0; return; } /* strcat */
    case (DLL_ESTLIB << 16) | 180: { int r = 0; for (u32 i = 0;; i++) { u8 x = rd8(a0 + i), y = rd8(a1 + i); if (x != y) { r = x < y ? -1 : 1; break; } if (!x) break; } R[0] = (u32)r; return; } /* strcmp */
    case (DLL_ESTLIB << 16) | 8:                                           /* memset */
        for (u32 i = 0; i < a2; i++) wr8(a0 + i, a1);
        R[0] = a0; return;
    case (DLL_ESTLIB << 16) | 181: {                                       /* strcpy */
        u32 i = 0; for (;;) { u8 c = rd8(a1 + i); wr8(a0 + i, c); if (!c) break; i++; }
        R[0] = a0; return; }
    case (DLL_ESTLIB << 16) | 174: {                                       /* strncmp */
        int r = 0; for (u32 i = 0; i < a2; i++) { u8 x = rd8(a0 + i), y = rd8(a1 + i); if (x != y) { r = x < y ? -1 : 1; break; } if (!x) break; }
        R[0] = (u32)r; return; }
    default: break;
    }
    if (verbose_unhandled)
        fatal("UNHANDLED import %s[%d] (slot %d) lr=%08x r0=%08x r1=%08x r2=%08x r3=%08x", EA_DLL_NAMES[dll], ord, slot, R[14], R[0], R[1], R[2], R[3]);
    {   static unsigned char warned[EA_NSLOTS];
        if (!warned[slot]) { warned[slot] = 1; snprintf(errbuf, sizeof errbuf, "ignored import %s[%d] lr=%08x", EA_DLL_NAMES[dll], ord, R[14]); }
    }
    R[0] = 0;
}

static u32 call_cb(int slot, u32 a1, u32 a2) {
    u32 vp = rd32(au_cb), fn = rd32(vp + 8 + 4 * slot);
    return call(fn - IMG_BASE, au_cb, a1, a2, 0, NULL, 0);
}

/* ------------------------------------------------------------------ public API */
static u32 G;              /* CGlobals* */
static u32 fake_container; /* object with [+0x48] = globals */
static u32 keybuf;

static void forward_reset_state(void);
int ea_init(const uint8_t *code, size_t code_len, const uint8_t *head, size_t head_len, size_t total) {
    /* fully re-entrant: a second launch inside the same process starts from scratch */
    free(img); free(heap); free(stk); free(freehead); free(pakhead_owned);
    img = heap = stk = NULL; freehead = NULL; pakhead_owned = NULL;
    for (int k = 0; k < MAX_ASSETS; k++) { if (asset_owned[k]) free((void *)asset_ptr[k]); asset_ptr[k] = NULL; asset_len[k] = 0; asset_owned[k] = 0; }
    cfg_len = 0; cfg_dirty = 0; icount = 0; heap_top = 0; errbuf[0] = 0;
    forward_reset_state();
    img = calloc(1, IMG_SIZE); heap = calloc(1, HEAP_SIZE); stk = calloc(1, STK_SIZE);
    freehead = calloc(NB, sizeof(u32));
    if (!img || !heap || !stk || !freehead) return -1;
    if (code_len > IMG_SIZE) return -2;
    memcpy(img, code, code_len);
    { u8 *ph = malloc(head_len + 1); if (!ph) return -3; memcpy(ph, head, head_len); pakhead = pakhead_owned = ph; }
    pakhead_len = (u32)head_len; pak_total = (u32)total;
    heap_top = 0;
    for (int k = 0; k < EA_NSLOTS; k++) wr32(IMG_BASE + EA_IAT_OFFSET + 4 * k, TRAP_BASE + 4 * k);
    memset(hooks, 0, sizeof hooks);
#define HOOK(off, fn) hooks[(off) >> 2] = (fn)
    HOOK(0x3e650, h_settls); HOOK(0x3e660, h_gettls);
    HOOK(0x277c0, h_alloc2); HOOK(0x278a8, h_free2);
    HOOK(0x271d4, h_alloc1); HOOK(0x271f4, h_free1);
#ifndef EA_NOBLIT
    HOOK(0x28468, h_tile_opaque); HOOK(0x28934, h_tile_trans); HOOK(0x28784, h_tile_add); HOOK(0x28c80, h_tile_add_t); HOOK(0x28d74, h_tile_sub_t); HOOK(0x3534, h_fxmul);
#endif
    HOOK(0xa7a0, h_nop); HOOK(0x1226c, h_idfmt); HOOK(0xab18, h_writefile); HOOK(0xa888, h_filesize); HOOK(0xa9f4, h_readat); HOOK(0xadb4, h_readindex);
    icount_limit = ~0ull;
    return 0;
}

static void forward_reset_state(void) {
    pthread_mutex_lock(&ring_mx); ring_r = ring_w = 0; pthread_mutex_unlock(&ring_mx);
    au_cb = au_stream = au_vt = 0; au_open_pending = au_write_done = 0; au_state = 0; au_last_desc = 0;
    tls_value = 0; G = 0; fake_container = 0; keybuf = 0;
}

void ea_reset(void) {                                /* forget all guest state (used to recover after an engine fault) */
    memset(heap, 0, heap_top + 64u < HEAP_SIZE ? heap_top + 64u : HEAP_SIZE);
    memset(freehead, 0, NB * sizeof(u32));
    heap_top = 0;
    pthread_mutex_lock(&ring_mx); ring_r = ring_w = 0; pthread_mutex_unlock(&ring_mx);
    au_cb = au_stream = au_vt = 0; au_open_pending = au_write_done = 0; au_state = 0; au_last_desc = 0;
    tls_value = 0; G = 0; fake_container = 0; keybuf = 0;
    memset(R, 0, sizeof R); jb_valid = 0;
}

int ea_boot(void) {
    icount_limit = icount + 2000000000ull;
    if (setjmp(jb)) { jb_valid = 0; return -1; }
    jb_valid = 1;
    G = halloc(0x398);
    call(0xc944, G, 0, 0, 0, NULL, 0);                    /* CGlobals constructor */
    tls_value = G;
    u32 path = halloc(0x300);
    wr32(G + 0x198, path);                                  /* app path descriptor (unused: file I/O is hooked) */
    u32 appui = halloc(0x400);  wr32(G + 0x18c, appui);
    fake_container = halloc(0x100); wr32(fake_container + 0x48, G); wr32(G + 0x190, fake_container);
    wr32(G + 0x19c, fake_container + 0x20);
    call(0xd6b8, G, 0, 0, 0, NULL, 0);                     /* Globals_InitPak */
    /* key object (container ConstructL, sub_012d00) */
    u32 key = halloc(0x58);
    call(0x11f78, key, 0, 0, 0, NULL, 0);
    wr32(G + 0x31c, key); wr32(key + 0x54, G);
    call(0x12134, key, 0, 0, 0, NULL, 0);
    u32 helper = halloc(0x40); wr32(helper + 0x20, 7);      /* display mode: EColor64K */
    wr32(G + 0x194, helper);
    wr8(G + 0x140, 1);                                      /* application in foreground */
    call(0xd940, G, 0, 0, 0, NULL, 0);                     /* creates frame bitmap, loads loading.pic + sounds */
    call(0xda18, G, 0, 0, 0, NULL, 0);                     /* loads "menu" theme */
    keybuf = halloc(32);
    jb_valid = 0;
    return 0;
}

int ea_frame(void) {
    icount_limit = icount + 300000000ull;
    if (setjmp(jb)) { jb_valid = 0; return -1; }
    jb_valid = 1;
    u32 bmp = rd32(G + 0x1a0);
    u32 data = rd32(bmp);                                   /* fake CFbsBitmap: [0] = data pointer */
    wr32(G + 0x320, data);
    call(0x7418, rd32(G + 0x318), data, 0, 0, NULL, 0);     /* renderer->screen = data */
    audio_service();
    u32 r = call(0xdd2c, G, 0, 0, 0, NULL, 0);              /* engine: update + draw one frame */
    if (rd32(G + 0x31c)) call(0x12160, rd32(G + 0x31c), 0, 0, 0, NULL, 0);   /* latch key state for edge detection (as the UI glue does) */
    jb_valid = 0;
    return (int)r;
}

int ea_key(int scancode, int down) {
    if (setjmp(jb)) { jb_valid = 0; return -1; }
    jb_valid = 1;
    wr32(keybuf, 0); wr32(keybuf + 4, (u32)scancode); wr32(keybuf + 8, 0); wr32(keybuf + 12, 0);
    u32 r = call(0x12f04, fake_container, keybuf, down ? 3 : 2, 0, NULL, 0);
    jb_valid = 0;
    return (int)r;
}

const uint16_t *ea_screen(int *w, int *h) {
    static uint16_t tmp[192 * 224];
    u32 bmp = rd32(G + 0x1a0), data = rd32(bmp);
    if (!data) return NULL;
    memcpy(tmp, mp(data), sizeof tmp);
    *w = 192; *h = 224;
    return tmp;
}
int ea_copy_screen(uint16_t *dst) {                 /* visible 176x208 area of the engine's 192x224 bitmap */
    u32 bmp = rd32(G + 0x1a0), data = rd32(bmp);
    if (!data) return -1;
    const u16 *src = (const u16 *)mp(data);
    for (int y = 0; y < 208; y++) memcpy(dst + y * 176, src + (y + 8) * 192 + 8, 176 * 2);
    return 0;
}
int ea_save_config(void) {                          /* GameCfg_Save(g+0x10): the same call the original makes at shutdown */
    if (!G) return -1;
    if (setjmp(jb)) { jb_valid = 0; return -1; }
    jb_valid = 1; icount_limit = icount + 50000000ull;
    call(0x22b28, G + 0x10, G, 0, 0, NULL, 0);
    jb_valid = 0; return 0;
}
int ea_quit_requested(void) { return G ? rd8(G + 0x35b) : 0; }
void ea_set_strict(int on) { verbose_unhandled = on; }
uint32_t ea_heap_used(void) { return heap_top; }
uint64_t ea_instr_count(void) { return icount; }
uint32_t ea_rd32(uint32_t a) { return rd32(a); }
uint32_t ea_globals(void) { return G; }
