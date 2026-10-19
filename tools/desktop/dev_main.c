/* dev harness: boots the engine on the desktop, runs a script, dumps frames as PPM */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ea_host.h"
#ifdef EA_PROFILE
void ea_prof_dump(const char *path);
#endif
static uint8_t *slurp(const char *p, size_t *n) { FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); } fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET); uint8_t *b = malloc(*n + 1); if (fread(b, 1, *n, f) != *n) exit(1); fclose(f); return b; }
static void dump(const char *path) {
    int w, h; const uint16_t *fb = ea_screen(&w, &h); if (!fb) { printf("no screen\n"); return; }
    FILE *f = fopen(path, "wb"); fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { uint16_t p = fb[i]; uint8_t c[3] = { (uint8_t)(((p >> 11) & 31) * 255 / 31), (uint8_t)(((p >> 5) & 63) * 255 / 63), (uint8_t)((p & 31) * 255 / 31) }; fwrite(c, 1, 3, f); }
    fclose(f);
}
static int16_t *rec; static size_t rec_n, rec_cap; static int rec_on;
static void rec_pull(void) {
    if (!rec_on) return;
    int rate = ea_audio_rate(); int n = rate / 32; int16_t tmp[4096]; if (n > 4096) n = 4096;
    if (getenv("ATRACE")) { static int fr; fr++; if (fr > 240 && fr < 280) printf("fr %d fill %u state %d\n", fr, ea_audio_fill(), ea_audio_state()); }
    ea_audio_read(tmp, n);
    { int z = 1; for (int i = 0; i < n; i++) if (tmp[i]) { z = 0; break; } static int run0; static int frame; frame++; if (z) run0++; else { if (run0 > 3) printf("silence %d frames ending at frame %d\n", run0, frame); run0 = 0; } }
    if (rec_n + n > rec_cap) { rec_cap = (rec_cap + n) * 2; rec = realloc(rec, rec_cap * 2); }
    memcpy(rec + rec_n, tmp, n * 2); rec_n += n;
}
static void wav_write(const char *p) {
    FILE *f = fopen(p, "wb"); uint32_t rate = ea_audio_rate(), dl = (uint32_t)rec_n * 2, v;
    fwrite("RIFF", 1, 4, f); v = 36 + dl; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    uint16_t h = 1; fwrite(&h, 2, 1, f); fwrite(&h, 2, 1, f); fwrite(&rate, 4, 1, f); v = rate * 2; fwrite(&v, 4, 1, f); h = 2; fwrite(&h, 2, 1, f); h = 16; fwrite(&h, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dl, 4, 1, f); fwrite(rec, 2, rec_n, f); fclose(f);
    printf("wav: %zu samples @%u Hz\n", rec_n, rate);
}
static unsigned long long frame_hash_all = 1469598103934665603ull; static int hashlog;
static void frame_hash(void) { int w, h; const uint16_t *fb = ea_screen(&w, &h); if (!fb) return; unsigned long long x = 1469598103934665603ull; const uint8_t *p = (const uint8_t *)fb; for (int i = 0; i < w * h * 2; i++) { x ^= p[i]; x *= 1099511628211ull; } if (hashlog) printf("H %016llx\n", x); frame_hash_all ^= x + 0x9e3779b97f4a7c15ull + (frame_hash_all << 6) + (frame_hash_all >> 2); }
int main(int argc, char **argv) {
    const char *dir = argv[1];            /* port/data */
    char p[512]; size_t n, hn;
    snprintf(p, sizeof p, "%s/code.bin", dir); uint8_t *code = slurp(p, &n);
    snprintf(p, sizeof p, "%s/pakhead.bin", dir); uint8_t *head = slurp(p, &hn);
    if (ea_init(code, n, head, hn, 412088)) { printf("init failed\n"); return 1; }
    for (int i = 0; i < 98; i++) { snprintf(p, sizeof p, "%s/%02d.bin", dir, i); size_t an; uint8_t *a = slurp(p, &an); ea_set_asset(i, a, (uint32_t)an); }
    FILE *sf = fopen(argv[2], "r"); char line[256]; uint32_t ticks = 0;
    while (fgets(line, sizeof line, sf)) {
        char cmd[32], arg[200] = ""; int a = 0, b = 0;
        if (sscanf(line, "%31s", cmd) != 1) continue;
        if (!strcmp(cmd, "audiolog")) ea_audio_log(1); else if (!strcmp(cmd, "hashlog")) hashlog = 1; else if (!strcmp(cmd, "boot")) { int r = ea_boot(); printf("boot -> %d %s (instr %llu)\n", r, r ? ea_last_error() : "", (unsigned long long)ea_instr_count()); if (r) return 2; }
        else if (!strcmp(cmd, "run")) { sscanf(line, "%*s %d", &a); for (int i = 0; i < a; i++) { ticks += 2; ea_set_ticks(ticks); uint64_t c0 = ea_instr_count(); int r = ea_frame(); { static unsigned long long maxframe; unsigned long long dc = ea_instr_count() - c0; if (dc > maxframe) { maxframe = dc; printf("new max frame cost: %llu instr\n", maxframe); } } frame_hash(); rec_pull(); if (r < 0) { printf("frame error: %s\n", ea_last_error()); return 3; } if (i == a - 1) printf("run: last frame ret=%d instr=%llu\n", r, (unsigned long long)(ea_instr_count() - c0)); } }
        else if (!strcmp(cmd, "key")) { sscanf(line, "%*s %i %d", &a, &b); int r = ea_key(a, b); if (r < 0) { printf("key error: %s\n", ea_last_error()); return 3; } }
        else if (!strcmp(cmd, "rec")) rec_on = 1; else if (!strcmp(cmd, "wav")) { sscanf(line, "%*s %199s", arg); wav_write(arg); }
        else if (!strcmp(cmd, "dump")) { sscanf(line, "%*s %199s", arg); dump(arg); }
        else if (!strcmp(cmd, "prof")) {
#ifdef EA_PROFILE
 sscanf(line, "%*s %199s", arg); ea_prof_dump(arg);
#endif
 }
        else if (!strcmp(cmd, "reset")) { ea_reset(); }
        else if (!strcmp(cmd, "strict")) { sscanf(line, "%*s %d", &a); ea_set_strict(a); }
        else if (!strcmp(cmd, "savecfg")) { printf("save -> %d\n", ea_save_config()); }
        else if (!strcmp(cmd, "cfg")) { uint32_t n; const uint8_t *c = ea_config(&n); int dirty = ea_config_dirty(); printf("cfg len %u dirty %d:", n, dirty); for (uint32_t i = 0; i < n && i < 24; i++) printf(" %02x", c[i]); printf("\n"); }
        else if (!strcmp(cmd, "heap")) printf("heap used %u KB, instr %llu, quit=%d\n", ea_heap_used()/1024, (unsigned long long)ea_instr_count(), ea_quit_requested());
        else if (!strcmp(cmd, "peek")) { sscanf(line, "%*s %i", &a); printf("[%08x] = %08x\n", a, ea_rd32(a)); }
    }
    printf("FINAL %016llx instr=%llu\n", frame_hash_all, (unsigned long long)ea_instr_count());
    return 0;
}
