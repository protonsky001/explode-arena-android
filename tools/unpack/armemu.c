// Minimal ARMv4 (ARM-state) interpreter used to run the game's own decompressors.
// Usage: armemu <app_code.bin> <iat_slots_file> <cmd...>   (see main)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef uint32_t u32; typedef int32_t s32; typedef uint8_t u8; typedef uint16_t u16; typedef uint64_t u64; typedef int64_t s64;

#define IMG_BASE   0x10000000u
#define IMG_SIZE   0x00060000u
#define HEAP_BASE  0x20000000u
#define HEAP_SIZE  0x01000000u
#define STK_BASE   0x30000000u
#define STK_SIZE   0x00100000u
#define BUF_BASE   0x40000000u
#define BUF_SIZE   0x01000000u
#define TRAP_BASE  0xF0000000u
#define HALT_ADDR  0xFFFF0000u

static u8 *img, *heap, *stk, *buf;
static u32 R[16];
static int fN, fZ, fC, fV;
static u32 heap_ptr = HEAP_BASE + 16;
static u64 icount = 0;
static int verbose = 0;
static const char **slot_names; // for diagnostics
static int nslots;

static inline u8 *mp(u32 a) {
    if (a - IMG_BASE < IMG_SIZE) return img + (a - IMG_BASE);
    if (a - HEAP_BASE < HEAP_SIZE) return heap + (a - HEAP_BASE);
    if (a - STK_BASE < STK_SIZE) return stk + (a - STK_BASE);
    if (a - BUF_BASE < BUF_SIZE) return buf + (a - BUF_BASE);
    fprintf(stderr, "BAD MEM ACCESS 0x%08x (pc=%08x lr=%08x) icount=%llu\n", a, R[15], R[14], (unsigned long long)icount);
    exit(3);
}
static inline u32 rd32(u32 a) { u32 v; memcpy(&v, mp(a & ~3u), 4); u32 r = (a & 3) * 8; return r ? (v >> r) | (v << (32 - r)) : v; }
static inline u32 rd16(u32 a) { u16 v; memcpy(&v, mp(a & ~1u), 2); return v; }
static inline u32 rd8(u32 a) { return *mp(a); }
static inline void wr32(u32 a, u32 v) { memcpy(mp(a & ~3u), &v, 4); }
static inline void wr16(u32 a, u32 v) { u16 w = (u16)v; memcpy(mp(a & ~1u), &w, 2); }
static inline void wr8(u32 a, u32 v) { *mp(a) = (u8)v; }

static u32 halloc(u32 n) {
    n = (n + 7) & ~7u;
    u32 p = heap_ptr; heap_ptr += n + 8;
    if (heap_ptr - HEAP_BASE > HEAP_SIZE) { fprintf(stderr, "heap exhausted\n"); exit(4); }
    memset(mp(p), 0, n);
    return p;
}

// ---- host-side import handlers: return 1 if handled ----
static int slot_dll_ord(int slot, char *dll, int *ord);

static u32 dsl_dll_ord[1024][2];
static char dsl_names[1024][32];

static void host_call(int slot) {
    const char *dll = dsl_names[slot]; int ord = (int)dsl_dll_ord[slot][1];
    u32 a0 = R[0], a1 = R[1], a2 = R[2];
    if (!strcmp(dll, "euser") && ord == 1505) { R[0] = halloc(a0); return; }        // operator new
    if (!strcmp(dll, "euser") && ord == 1504) { R[0] = 0; return; }                 // operator delete
    if (!strcmp(dll, "euser") && ord == 2) { for (u32 i = 0; i < a2; i++) wr8(a0 + i, rd8(a1 + i)); R[0] = a0 + a2; return; } // Mem::Copy
    if (!strcmp(dll, "estlib") && ord == 181) { u32 i = 0; do { u8 c = rd8(a1 + i); wr8(a0 + i, c); if (!c) break; i++; } while (1); R[0] = a0; return; } // strcpy
    if (!strcmp(dll, "estlib") && ord == 174) { int r = 0; for (u32 i = 0; i < a2; i++) { u8 x = rd8(a0 + i), y = rd8(a1 + i); if (x != y) { r = x < y ? -1 : 1; break; } if (!x) break; } R[0] = (u32)r; return; } // strncmp
    if (!strcmp(dll, "euser") && ord == 1514) { // integer divide helper: r0 / r1 (signed)
        s32 n = (s32)a0, d = (s32)a1; R[0] = d ? (u32)(n / d) : 0; return; }
    if (!strcmp(dll, "estlib") && ord == 8) { for (u32 i = 0; i < a2; i++) wr8(a0 + i, a1); R[0] = a0; return; }   // memset
    if (!strcmp(dll, "euser") && ord == 45) { R[0] = halloc(a0); return; }          // User::Alloc
    if (!strcmp(dll, "euser") && ord == 476) { R[0] = 0; return; }                  // User::Free
    if (!strcmp(dll, "euser") && ord == 1616) { u32 n = a0, d = a1; if (!d) { R[0] = 0; return; } R[0] = n / d; R[1] = n % d; return; } // unsigned divide helper
    fprintf(stderr, "UNHANDLED IMPORT %s[%d] slot %d pc=%08x lr=%08x r0=%08x r1=%08x r2=%08x r3=%08x icount=%llu\n", dll, ord, slot, R[15], R[14], R[0], R[1], R[2], R[3], (unsigned long long)icount);
    exit(5);
}

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
    u32 type = (i >> 5) & 3, amt;
    int c = fC;
    if (i & 0x10) { // reg shift
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
static inline u32 subc(u32 a, u32 b, u32 c, int s) { // a - b - !c  == a + ~b + c
    u64 r = (u64)a + (u32)~b + c; u32 res = (u32)r;
    if (s) { setNZ(res); fC = (r >> 32) & 1; fV = ((a ^ b) & (a ^ res)) >> 31; }
    return res;
}

static void write_pc(u32 v) { R[15] = v & ~3u; }

static void step(void) {
    u32 pc = R[15];
    if (pc >= TRAP_BASE && pc < TRAP_BASE + 0x10000) { host_call((pc - TRAP_BASE) / 4); R[15] = R[14] & ~1u; return; }
    if (pc == 0x100271d4u) { R[0] = halloc(R[0]); R[15] = R[14] & ~1u; return; }   // sub_0271d4 == game malloc
    if (pc == 0x100271f4u) { R[15] = R[14] & ~1u; return; }                          // sub_0271f4 == game free
    u32 i = rd32(pc);
    R[15] = pc + 4;
    icount++;
    if (verbose) fprintf(stderr, "%08x: %08x\n", pc, i);
    u32 cond = i >> 28;
    if (!cond_ok(cond)) return;
    u32 pcv = pc + 8;
    switch ((i >> 25) & 7) {
    case 0:
    case 1: {
        if ((i & 0x0ffffff0) == 0x012fff10) { u32 t = R[i & 15]; if (t & 1) { fprintf(stderr, "THUMB switch at %08x\n", pc); exit(6); } R[15] = t & ~3u; return; }
        if (((i >> 25) & 7) == 0 && (i & 0x90) == 0x90) {
            if ((i & 0x60) == 0) { // multiply / swap
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
                if ((i & 0x0fb000f0) == 0x01000090) { // swp
                    u32 rn = (i >> 16) & 15, rd = (i >> 12) & 15, rm = i & 15; u32 a = R[rn];
                    if (i & 0x400000) { u32 t = rd8(a); wr8(a, R[rm]); R[rd] = t; } else { u32 t = rd32(a); wr32(a, R[rm]); R[rd] = t; }
                    return;
                }
            } else { // halfword / signed
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
                    if (sh != 1) { fprintf(stderr, "ldrd/strd unsupported at %08x\n", pc); exit(6); }
                    wr16(addr, R[rd]);
                    if ((!P) || W) R[rn] = P ? addr : (U ? base + off : base - off);
                }
                return;
            }
        }
        // PSR transfers / misc unsupported
        u32 op = (i >> 25 & 1) ? 0 : 0; (void)op;
        u32 opc = (i >> 21) & 15, S = i & 0x100000;
        if (opc >= 8 && opc <= 11 && !S) { fprintf(stderr, "PSR/misc op %08x at %08x unsupported\n", i, pc); exit(6); }
        u32 rn = (i >> 16) & 15, rd = (i >> 12) & 15;
        u32 a = (rn == 15) ? ((i & 0x2000000) ? pcv : (((i & 0x10)) ? pcv + 4 : pcv)) : R[rn];
        u32 b; int sc = fC;
        if (i & 0x2000000) { u32 imm = i & 0xff, rot = ((i >> 8) & 15) * 2; b = ror(imm, rot); if (rot) sc = b >> 31; }
        else b = shifter(i, pcv, &sc);
        u32 res = 0; int wb = 1; int logical = 0;
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
        if (((i >> 25) & 7) == 3 && (i & 0x10)) { fprintf(stderr, "undefined %08x at %08x\n", i, pc); exit(6); }
        u32 L = i & 0x100000, B = i & 0x400000, P = i & 0x1000000, U = i & 0x800000, W = i & 0x200000;
        u32 rn = (i >> 16) & 15, rd = (i >> 12) & 15;
        u32 off;
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
        fprintf(stderr, "unsupported instruction %08x at %08x\n", i, pc);
        exit(6);
    }
}

static u32 call(u32 addr, u32 a0, u32 a1, u32 a2, u32 a3, const u32 *stackargs, int nstack) {
    R[0] = a0; R[1] = a1; R[2] = a2; R[3] = a3;
    u32 sp = STK_BASE + STK_SIZE - 0x1000;
    for (int k = 0; k < nstack; k++) wr32(sp + 4 * k, stackargs[k]);
    R[13] = sp; R[14] = HALT_ADDR; R[15] = addr;
    while (R[15] != HALT_ADDR) step();
    return R[0];
}

int main(int argc, char **argv) {
    // argv: code.bin slots.txt  pak_entries... handled via commands on stdin
    img = calloc(1, IMG_SIZE); heap = calloc(1, HEAP_SIZE); stk = calloc(1, STK_SIZE); buf = calloc(1, BUF_SIZE);
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror("code"); return 1; }
    size_t n = fread(img, 1, IMG_SIZE, f); fclose(f); (void)n;
    // slots: "iat_offset dll ordinal" per line
    f = fopen(argv[2], "r"); char dll[64]; int off, ord, k = 0;
    while (fscanf(f, "%d %63s %d", &off, dll, &ord) == 3) {
        strncpy(dsl_names[k], dll, 31); dsl_dll_ord[k][1] = ord;
        wr32(IMG_BASE + off, TRAP_BASE + 4 * k);
        k++;
    }
    fclose(f);
    // commands on stdin:  "T2 <in> <out> <csz> <usz> <outfile>"  etc.
    char cmd[16];
    while (scanf("%15s", cmd) == 1) {
        if (!strcmp(cmd, "LZARI") || !strcmp(cmd, "IMP")) {
            char inf[256], outf[256]; u32 csz, usz;
            if (scanf("%255s %255s %u %u", inf, outf, &csz, &usz) != 4) return 2;
            FILE *fi = fopen(inf, "rb"); if (!fi) { perror(inf); return 2; }
            u32 src = BUF_BASE, dst = BUF_BASE + 0x400000;
            size_t got = fread(mp(src), 1, csz, fi); fclose(fi); (void)got;
            memset(mp(dst), 0, usz + 16);
            heap_ptr = HEAP_BASE + 16;
            icount = 0;
            if (!strcmp(cmd, "LZARI")) {
                u32 st = halloc(0x6424);
                call(0x10017eac, st, src, dst, 0, NULL, 0);      // sub_017eac(state, src, dst)
            } else {
                u32 st = halloc(0x8878);
                call(0x1000f7b4, st, 0, 0, 0, NULL, 0);          // constructor (CRC table etc.)
                u32 sa[1] = { usz };
                u32 rc = call(0x10011bc8, st, src, dst, csz, sa, 1); // sub_011bc8(state, src, dst, csz, usz)
                if (rc != 0) fprintf(stderr, "IMP rc=%d\n", (int)rc);
            }
            FILE *fo = fopen(outf, "wb"); fwrite(mp(dst), 1, usz, fo); fclose(fo);
            printf("OK %s %llu instr\n", outf, (unsigned long long)icount); fflush(stdout);
        } else if (!strcmp(cmd, "QUIT")) break;
    }
    return 0;
}
