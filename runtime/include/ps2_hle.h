#ifndef PS2_HLE_H
#define PS2_HLE_H

#include "ps2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HA(n)   ((u32)ctx->r[(n) < 4 ? 4 + (n) : 8 + ((n) - 4)].ud[0])
#define HAS(n)  ((s32)HA(n))
#define HSTK(i) (ps2_r32((u32)ctx->r[29].ud[0] + 4u * (u32)(i)))
#define HRET(v)   do { ctx->r[2].sd[0] = (s64)(s32)(v); } while (0)
#define HRET64(v) do { ctx->r[2].ud[0] = (u64)(v); } while (0)

PS2_INLINE u32 ps2_arg(ps2_ctx *ctx, int i) {
    if (i < 4) return (u32)ctx->r[4 + i].ud[0];
    if (i < 8) return (u32)ctx->r[8 + (i - 4)].ud[0];
    return ps2_r32((u32)ctx->r[29].ud[0] + 4u * (u32)(i - 8));
}

void ps2_get_str(u32 addr, char *dst, size_t cap);
void ps2_put_mem(u32 addr, const void *src, size_t n);
void ps2_get_mem(void *dst, u32 addr, size_t n);

typedef struct ps2_disc_file {
    char name[256];
    u32 lsn;
    u32 size;
    u8  date[8];
    u32 flag;
} ps2_disc_file;

typedef int (*ps2_rpc_fn)(ps2_ctx *ctx, u32 fno, u32 send, int ssize,
                          u32 recv, int rsize);

void ps2_rpc_register(u32 sid, ps2_rpc_fn fn, const char *name);
int  ps2_rpc_call(u32 sid, u32 fno, u32 send, int ssize,
                  u32 recv, int rsize);
void ps2_sif_hle_init(void);
void ps2_sif_hle_report(void);

void ps2_cdvd_init(void);
void ps2_cdvd_report(void);
int  ps2_cdvd_sync_pending(void);

#define PS2_PAD_PORTS 2
#define PS2_PAD_ANALOG_NEUTRAL 0x7F
typedef struct ps2_pad_state {
    int connected;
    u16 buttons;
    u8 lx, ly, rx, ry;
    u8 l2, r2;
} ps2_pad_state;
extern ps2_pad_state ps2_pad_host[PS2_PAD_PORTS];

void ps2_dbc_report(u32 obj, u32 recv, int rsize);
void ps2_stop_scene(int major, int minor, int frames);
void ps2_stop_scene_check(unsigned major, unsigned minor);
void ps2_pad_init(void);
void ps2_pad_report(void);
void ps2_pad_io_report(void);
void ps2_pad_autoplay(int on);
void ps2_gs_deinterlace(int off);
void ps2_cdvd_rpc_register(void);
void ps2_iop_services_register(void);
void ps2_nufile_report(void);
void ps2_nusound_report(void);
void ps2_nusndstr_report(void);

void ps2_ipu_init(void);
u32  ps2_ipu_read32(u32 addr);
void ps2_ipu_write32(u32 addr, u32 value);
void ps2_ipu_dma_in(const void *qw, u32 n);
void ps2_ipu_dma_out(void *dst, u32 n);
u32  ps2_ipu_in_space(void);
u32  ps2_ipu_out_avail(void);
u32  ps2_ipu_want(void);
u32  ps2_ipu_out_pos(void);
void ps2_dmac_ipu_pull(void);
void ps2_dmac_ipu_drain(void);
void ps2_dmac_ipu_report(u32 *chcr, u32 *madr, u32 *qwc, u32 *tadr);
void ps2_dmac_ipu_history(void);
void ps2_dmac_ipu_out_history(void);
void ps2_gs_trx_history(void);
u32  ps2_dmac_pending(void);
void ps2_dmac_ack(int ch);
void ps2_ipu_report(void);
int  ps2_ipu_selftest(const char *path, int max_frames);

void ps2_video_hidden(int on);
int  ps2_video_init(const char *title, int width, int height);
void ps2_video_shutdown(void);
int  ps2_video_frame(void);
void ps2_video_report(void);

typedef void (*ps2_state_put)(void *ud, const char *tag,
                              const void *p, u64 n);

void ps2_gs_state_save(ps2_state_put put, void *ud);
void ps2_hw_state_save(ps2_state_put put, void *ud);
void ps2_kernel_state_save(ps2_state_put put, void *ud);
void ps2_ipu_state_save(ps2_state_put put, void *ud);
void ps2_vk_state_save(ps2_state_put put, void *ud);
void ps2_vu_state_save(ps2_state_put put, void *ud);
void ps2_state_dump(const char *path);
void ps2_state_dump_arm(const char *path);
void ps2_state_dump_if_armed(void);
void ps2_state_request_dump(void);
int  ps2_state_dump_pending(void);
void ps2_state_dump_numbered(const char *note);

#ifdef __cplusplus
}
#endif

#endif
