#ifndef EA_HOST_H
#define EA_HOST_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int  ea_init(const uint8_t *code, size_t code_len, const uint8_t *pakhead, size_t head_len, size_t pak_total);
void ea_set_asset(int idx, const uint8_t *data, uint32_t len);
void ea_set_asset_copy(int idx, const uint8_t *data, uint32_t len);
void ea_set_ticks(uint32_t t);
void ea_reset(void);
int  ea_boot(void);
int  ea_frame(void);
int  ea_key(int scancode, int down);
const uint16_t *ea_screen(int *w, int *h);
const char *ea_last_error(void);
uint64_t ea_instr_count(void);
uint32_t ea_rd32(uint32_t a);
uint32_t ea_globals(void);
const uint8_t *ea_config(uint32_t *len);
void ea_set_config(const uint8_t *d, uint32_t len);
int  ea_config_dirty(void);
int  ea_audio_read(int16_t *out, int n);
int  ea_copy_screen(uint16_t *dst);
int  ea_quit_requested(void);
int  ea_save_config(void);
void ea_set_strict(int on);
uint32_t ea_heap_used(void);
int  ea_audio_rate(void);
void ea_audio_log(int on);
unsigned ea_audio_fill(void);
int ea_audio_state(void);
#ifdef __cplusplus
}
#endif
#endif
