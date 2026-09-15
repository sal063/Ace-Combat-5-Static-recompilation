#ifndef PS2_CAPTURE_H
#define PS2_CAPTURE_H

#include "ps2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PS2_CAP_MAGIC   "GSCAP2"
#define PS2_CAP_VERSION 2u

enum {
    PS2_CAP_LEVEL_GS  = 0,
    PS2_CAP_LEVEL_DMA = 1
};

#define PS2_CAP_GS_PAGES 512u
#define PS2_CAP_RT_SLOTS 32u

typedef struct {
    u64 reg[0x64];
    u64 priv[0x200];
    u64 csr, imr;
    u32 rt_base[PS2_CAP_RT_SLOTS];
    u32 rt_epoch[PS2_CAP_RT_SLOTS];
    u32 rt_pages[PS2_CAP_RT_SLOTS];
    u32 rt_zbase[PS2_CAP_RT_SLOTS];
    u32 rt_zepoch[PS2_CAP_RT_SLOTS];
    u32 rt_n;
    u32 disp_base;
    u32 epoch_seq;
    u32 frame_count;
    u32 page_epoch[PS2_CAP_GS_PAGES];
    u32 page_owner[PS2_CAP_GS_PAGES];
    u16 page_owner_bw[PS2_CAP_GS_PAGES];
    u8  page_owner_psm[PS2_CAP_GS_PAGES];
    u8  pad[4];
} ps2_gs_capstate;

#define PS2_CAP_VIF_BLOB 512u
typedef struct {
    ps2_vu vu1, vu0;
    u8  vif[2][PS2_CAP_VIF_BLOB];
    u32 vif_size;
    u32 pad;
} ps2_vu_capstate;

typedef struct {
    u32 active, flg, nreg, nloop, eop, n, r;
    u64 regs;
    u32 q_latch;
    u32 path;
    u32 ctrl, mode, stat;
    u32 pad;
} ps2_gif_capstate;

typedef struct {
    char magic[8];
    u32  version;
    u32  level;
    u32  gs_state_size;
    u32  vu_state_size;
    u32  gif_state_size;
    u32  vram_size;
    u32  spr_size;
    u32  vu1_mem_size, vu1_micro_size, vu0_mem_size, vu0_micro_size;
    u64  mem_size;
    u32  blk_shift;
    u32  nblk;
    u32  dict_slots;
    u32  dict_min;
    u32  dict_max;
    u64  dict_budget;
    u64  stream_off;
    u64  stream_bytes;
    u64  index_off;
    u64  nrec;
    u32  nfields;
    u32  first_field;
    u32  truncated;
    u32  live_us_per_field;
    u32  reserved[8];
} ps2_cap_hdr;

typedef struct {
    u64 off;
    u32 field;
    u32 pad;
} ps2_cap_ixent;

enum {
    PS2_CAP_OP_REG    = 1,
    PS2_CAP_OP_HWRUN  = 2,
    PS2_CAP_OP_PRIV   = 3,
    PS2_CAP_OP_VIFW   = 4,
    PS2_CAP_OP_VIFQW  = 5,
    PS2_CAP_OP_GIFQW  = 6,
    PS2_CAP_OP_VIFREF = 7,
    PS2_CAP_OP_GIFREF = 8,
    PS2_CAP_OP_FBRST  = 9,
    PS2_CAP_OP_MMIO   = 10,
    PS2_CAP_OP_FIELD  = 11,
    PS2_CAP_OP_STATS  = 12,
    PS2_CAP_OP_END    = 13,
    PS2_CAP_OP_VIFCLR = 14
};

extern int g_cap_recording;
extern int g_cap_deep;
extern int g_cap_shallow;
extern int g_cap_disable;

void ps2_cap_maybe_start(void);

void ps2_cap_request(const char *path, int frames);
void ps2_cap_request_ex(const char *path, int frames, int level);

void ps2_cap_toggle(void);
void ps2_cap_stop(const char *why);
int  ps2_cap_active(void);
void ps2_cap_configure(const char *path, int frames, int level);
void ps2_cap_arm_at(u32 field);
void ps2_cap_report(void);

void ps2_cap_reg(u32 reg, u64 val);
void ps2_cap_priv(u32 addr, u64 val);
void ps2_cap_present(void);
void ps2_cap_vif_qw(int ch, const void *qw, u32 qwc);
void ps2_cap_gif_qw(const void *qw, u32 qwc);
void ps2_cap_vif_word(int ch, u32 word);
void ps2_cap_vif_reset(int ch, u32 val);
void ps2_cap_vif_clear(int ch);
void ps2_cap_mmio(u32 addr, u32 val);

void ps2_mkdir_p(const char *path);

void ps2_cap_dict_config(u32 slots, u32 lo, u32 hi, u64 budget);
void ps2_cap_dict_reset(void);
int  ps2_cap_dict_eligible(u32 len);
int  ps2_cap_dict_lookup(const void *p, u32 len, u32 *slot);
void ps2_cap_dict_insert(const void *p, u32 len);
const u8 *ps2_cap_dict_get(u32 slot, u32 *len);
void ps2_cap_dict_stats(u64 *hits, u64 *inserts, u64 *bytes_held,
                        u64 *bytes_saved);

void ps2_gs_cap_save(ps2_gs_capstate *st);
void ps2_gs_cap_load(const ps2_gs_capstate *st);
u8  *ps2_gs_vram_ptr(u32 *size);

void ps2_vu_cap_save(ps2_vu_capstate *st);
void ps2_vu_cap_load(const ps2_vu_capstate *st);
u8  *ps2_vu_cap_mem(int which, u32 *size);
u8  *ps2_vu_cap_micro(int which, u32 *size);

void ps2_gif_cap_save(ps2_gif_capstate *st);
void ps2_gif_cap_load(const ps2_gif_capstate *st);

u32  ps2_gs_field_parity(void);
void ps2_gs_set_field_parity(u32 odd);

#ifdef __cplusplus
}
#endif

#endif
