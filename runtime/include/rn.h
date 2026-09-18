#ifndef RN_H
#define RN_H

#include "ps2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RN_EMIT_NONE      = 0,
    RN_EMIT_DRAWCTRL  = 1,
    RN_EMIT_OTHEAD    = 2,
    RN_EMIT_OFFCHAIN  = 3,
    RN_EMIT_UNOWNED   = 4,
    RN_EMIT_FIRST     = 8,
    RN_EMIT_MAX       = 4096
};

enum {
    RN_TAG_EMITTER = 0,
    RN_TAG_SCENE   = 1,
    RN_TAG_DEFINE  = 2
};

enum {
    RN_VIA_OT      = 0,
    RN_VIA_2D      = 1,
    RN_VIA_DIRECT  = 2,
    RN_VIA_WRITER  = 3
};

void rn_init(void);

extern int rn_taps_on;

void rn_dma_attrib_slow(int ch, u32 tadr);
static inline void rn_dma_attrib(int ch, u32 tadr) {
    if (rn_taps_on) rn_dma_attrib_slow(ch, tadr);
}

typedef struct rn_emitter {
    u32 site;
    u32 func;
    u32 via;
    u32 bucket;
    u32 table;
    u64 ranges;
    u64 bytes;
} rn_emitter;

u32               rn_emitter_count(void);
const rn_emitter *rn_emitter_get(u32 id);
void              rn_emitter_define(u32 id, u32 site, u32 func, u32 via);
const char       *rn_emitter_name(u32 id, char *buf, size_t cap);

extern u32 rn_gs_emitter;

void rn_gs_tag(u32 kind, u32 a, u32 b);

enum { RN_PRIM_POINT = 0, RN_PRIM_LINE, RN_PRIM_TRI, RN_PRIM_SPRITE };
struct ps2_vk_state;
struct ps2_vk_vertex;
extern int rn_census_on;
void rn_census_prim_slow(const struct ps2_vk_state *st, int kind,
                         const struct ps2_vk_vertex *v, int n, int native);
static inline void rn_census_prim(const struct ps2_vk_state *st, int kind,
                                  const struct ps2_vk_vertex *v, int n, int native) {
    if (rn_census_on) rn_census_prim_slow(st, kind, v, n, native);
}
void rn_census_frame(void);

extern int rn_dump_on;
void rn_dump_init(void);
void rn_dump_frame(void);
void rn_dump_tag(u32 kind, u32 a, u32 b);
void rn_dump_vif(u32 code, u32 tops, u32 cl, u32 wl, u32 tpc, const u8 *mem,
                 const u8 *micro);
void rn_dump_xgkick(u32 addr, const u8 *mem, u32 mem_size);
void rn_dump_gsreg(u32 reg, u64 val);

extern int rn_vp_mode;
extern int rn_vp_dirty;
void rn_vp_init(void);
int  rn_vp_run(struct ps2_vu *vu, u32 start);
void rn_vp_kick(struct ps2_vu *vu, u32 addr);
void rn_vp_report(void);
int  rn_vp_upload(struct ps2_vu *vu, u32 start);
void rn_vp_verify_stats(int prog, u64 out[7]);
struct ps2_vu *ps2_vu1_test_vu(void);
void ps2_vu1_test_run(u32 start);
int  rn_vpfuzz_main(u32 start, u32 iterations, u32 seed);
int rn_vp_resident_slot(void);
const char *rn_vp_prog_name(int slot);
extern int rn_census_vu_prog;
extern int rn_census_native_prog;
extern int rn_vu_kicking;

typedef struct rn_intent_hdr {
    u16 kind;
    u16 flags;
    u32 len;
    u32 site;
    u32 emitter;
} rn_intent_hdr;

enum {
    RN_INT_CLAIM_END = 1,
    RN_INT_PRIM2D    = 2,
    RN_INT_GROUP2D   = 3,
    RN_INT_SKYDOME   = 4,
    RN_INT_TRI3D     = 5,
    RN_INT_BILLBOARD = 6,
    RN_INT_WORLDTRI  = 7
};

#define RN_G_WORLD 1u

typedef struct rn_int_prim2d {
    u32 prim;
    u32 count;
    u64 tex0;
} rn_int_prim2d;

typedef struct rn_vtx2d {
    u8  r, g, b, a;
    float s, t, q;
    u16 u, v;
    s32 x, y;
    u32 z;
    u32 pad;
} rn_vtx2d;

typedef struct rn_int_skydome {
    float screen[16];
    float clip[16];
    float seg[64];
    u32 rings, segs;
    u32 pass;
    u32 ring0, ring1;
    u32 layers;
    u32 z[8];
} rn_int_skydome;

enum { RN_SKY_DOME = 0, RN_SKY_HAZE = 1 };

typedef struct rn_int_worldtri {
    float rows[16];
    u32 z;
    u32 n;
} rn_int_worldtri;

typedef struct rn_world_vtx {
    float pos[4];
    float uv[2];
    u32 rgba;
    u32 pad;
} rn_world_vtx;

typedef struct rn_int_billboard {
    float rows[16];
    float pos[4];
    float size;
    float scale[2];
    u32 zmin;
    u16 uv[4];
    u32 rgba;
    u32 pad;
} rn_int_billboard;

typedef struct rn_int_tri3d {
    float screen[16];
    float clip[16];
    u32 prim;
    u32 pad;
    float colour[12];
    float uv[12];
    float pos[12];
} rn_int_tri3d;

typedef struct rn_sky_ring {
    float radius, height;
    u32 rgba;
    u32 pad;
} rn_sky_ring;

void rn_gs_intent(const u8 *rec, u32 len);

extern int rn_intents_pending;
void rn_dma_transfer_slow(int ch, u32 madr, u32 qwc, int after);
static inline void rn_dma_transfer(int ch, u32 madr, u32 qwc, int after) {
    if (rn_intents_pending) rn_dma_transfer_slow(ch, madr, qwc, after);
}

extern int rn_claims_on;
int rn_claim_prim_slow(const struct ps2_vk_state *st, int kind,
                       const struct ps2_vk_vertex *v, int n);
static inline int rn_claim_prim(const struct ps2_vk_state *st, int kind,
                                const struct ps2_vk_vertex *v, int n) {
    return rn_claims_on ? rn_claim_prim_slow(st, kind, v, n) : 0;
}
void rn_claims_init(void);
void rn_sun_report(void);

void rn_intent_init(void);
void rn_fixes_init(void);

void rn_report(void);
void rn_census_report(const char *why);
void rn_tap_report(void);
void rn_intent_report(void);
void rn_fixes_report(void);

#ifdef __cplusplus
}
#endif

#endif
