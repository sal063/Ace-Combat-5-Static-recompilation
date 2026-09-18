#ifndef PS2_GFXQ_H
#define PS2_GFXQ_H

#include "ps2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int ps2_gfxq_on;

void ps2_gfxq_init(void);

#define PS2_VIF_WORD_AT(ch, lane) ((ch) | 0x80 | ((lane) << 1))
void ps2_gfxq_forbid(const char *why);
void ps2_gfxq_shutdown(void);
void ps2_gfxq_report(void);

void ps2_gfxq_drain(void);

int  ps2_gfxq_blocks_capture(void);

void ps2_gfxq_vif_qw(int ch, const ps2_reg128 *qw, u32 qwc);
void ps2_gfxq_gif_qw(const ps2_reg128 *qw, u32 qwc);
void ps2_gfxq_vif_word(int ch, u32 word);
void ps2_gfxq_fbrst(int ch, u32 val);
void ps2_gfxq_vif_clear(int ch);
void ps2_gfxq_gs_priv(u32 addr, u64 val);
void ps2_gfxq_mmio(u32 addr, u32 val);
void ps2_gfxq_tag(u32 kind, u32 a, u32 b);
void ps2_gfxq_intent(const u8 *rec, u32 len);
int  ps2_gfxq_field(void);

#ifdef __cplusplus
}
#endif

#endif
