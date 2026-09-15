#ifndef PS2_RUNTIME_H
#define PS2_RUNTIME_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

#if defined(__clang__) && __clang_major__ >= 13
#  define PS2_TAIL __attribute__((musttail))
#elif defined(__GNUC__) && __GNUC__ >= 15
#  define PS2_TAIL [[gnu::musttail]]
#else
#  define PS2_TAIL
#  define PS2_NO_MUSTTAIL 1
#endif

#if defined(__GNUC__)
#  define PS2_ENV(name) __extension__ ({                                       \
       static int ps2_env_cache_ = -1;                                         \
       if (ps2_env_cache_ < 0) ps2_env_cache_ = getenv(name) != NULL;          \
       ps2_env_cache_; })
#else
#  define PS2_ENV(name) (getenv(name) != NULL)
#endif

#if defined(__GNUC__)
#  define PS2_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define PS2_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define PS2_INLINE      static inline __attribute__((always_inline))
#else
#  define PS2_LIKELY(x)   (x)
#  define PS2_UNLIKELY(x) (x)
#  define PS2_INLINE      static inline
#endif

typedef union ps2_reg128 {
    u64 ud[2];
    s64 sd[2];
    u32 uw[4];
    s32 sw[4];
    u16 uh[8];
    s16 sh[8];
    u8  ub[16];
    s8  sb[16];
    float fp[4];
} __attribute__((aligned(16))) ps2_reg128;

typedef union ps2_fpr {
    float f;
    u32 u;
    s32 s;
} ps2_fpr;

typedef union ps2_vf {
    float f[4];
    u32 u[4];
    s32 s[4];
} __attribute__((aligned(16))) ps2_vf;

#define PS2_VU0_MEM_SIZE   (4 * 1024)
#define PS2_VU0_MICRO_SIZE (4 * 1024)
#define PS2_VU1_MEM_SIZE   (16 * 1024)
#define PS2_VU1_MICRO_SIZE (16 * 1024)

typedef struct ps2_vu {
    ps2_vf vf[32];
    ps2_vf acc;
    u16 vi[16];
    float q, p, i;
    u32 r;
    u32 status, mac, clip;
    struct { u32 mac, status; int valid; } fpipe[4];
    u32 fslot;
    float q_pend, p_pend;
    u32 q_ready, p_ready;
    u32 top, itop;
    u32 cmsar;
    u32 tpc;
    u32 cc;
    u32 running;
    u8 *mem;
    u8 *micro;
    u32 mem_size, micro_size;
} ps2_vu;

typedef struct ps2_ctx {
    ps2_reg128 r[32];
    ps2_reg128 hi, lo;
    ps2_fpr f[32];
    float acc;
    u32 fcc;
    u32 fcr31;
    u32 sa;
    u32 pc;
    u32 cop0[32];
    ps2_vu vu0;
    struct { u32 mac; } vu0_flags[5];
    u32 vu0_flag_slot, vu0_flag_pending;
    u32 current_thread;
    u32 in_interrupt;
} ps2_ctx;

extern const ps2_reg128 ps2_zero_q;

typedef void (*ps2_fn)(ps2_ctx *ctx);

typedef struct ps2_func_entry {
    u32 addr;
    ps2_fn fn;
} ps2_func_entry;

extern const ps2_func_entry ps2_func_table[];
extern const unsigned ps2_func_count;
extern const u32 ps2_entry_point;

typedef struct ps2_symbol {
    u32 addr;
    const char *name;
} ps2_symbol;
extern const ps2_symbol ps2_symbols[];
extern const unsigned ps2_symbol_count;
const char *ps2_symbol_name(u32 addr);

#ifndef PS2_TRACE_CALLS
#  define PS2_TRACE_CALLS 1
#endif
#define PS2_TRACE_RING 4096

#ifndef PS2_DIAG
#  define PS2_DIAG 0
#endif

#if PS2_TRACE_CALLS
extern u32 ps2_trace_ring[PS2_TRACE_RING];
extern u32 ps2_trace_pos;
extern u32 *ps2_prof;
extern u32 ps2_prof_base, ps2_prof_size;
#  if PS2_DIAG
#    define PS2_ENTER_PROF(a) \
         if (ps2_prof && (u32)(a) - ps2_prof_base < ps2_prof_size) \
             ps2_prof[((u32)(a) - ps2_prof_base) >> 2]++;
#  else
#    define PS2_ENTER_PROF(a)
#  endif
#  define PS2_ENTER(a) \
      do { ps2_trace_ring[ps2_trace_pos++ & (PS2_TRACE_RING - 1)] = (a); \
           PS2_ENTER_PROF(a) \
           if (!(ps2_trace_pos & 0xFFFu)) ps2_preempt(); } while (0)
#else
#  define PS2_ENTER(a) ((void)0)
#endif
void ps2_dump_trace(const char *why);
void ps2_preempt(void);

extern u32 ps2_loop_ctr;
void ps2_loop_service(void);
#define PS2_LOOP()     do { if (!(++ps2_loop_ctr & 0xFFu)) ps2_loop_service(); } while (0)
void ps2_prof_enable(u32 base, u32 size);
void ps2_prof_report(unsigned top);

#define PS2_RAM_SIZE      0x02000000u
#define PS2_SPR_BASE      0x70000000u
#define PS2_SPR_SIZE      0x00004000u
#define PS2_IOP_RAM_BASE  0x1C000000u
#define PS2_IOP_RAM_SIZE  0x00200000u
#define PS2_BIOS_BASE     0x1FC00000u
#define PS2_BIOS_SIZE     0x00400000u

#define PS2_PAGE_BITS  12
#define PS2_PAGE_SIZE  (1u << PS2_PAGE_BITS)
#define PS2_PAGE_MASK  (PS2_PAGE_SIZE - 1u)
#define PS2_PT_ENTRIES (1u << (32 - PS2_PAGE_BITS))

extern u8 *ps2_pt[PS2_PT_ENTRIES];
extern u8 *ps2_ram;
extern u8 *ps2_spr;
extern u8 *ps2_iop_ram;

void ps2_mem_init(void);
void ps2_mem_map(u32 vaddr, u32 size, u8 *host);

u8   ps2_mmio_r8(u32 a);
u16  ps2_mmio_r16(u32 a);
u32  ps2_mmio_r32(u32 a);
u64  ps2_mmio_r64(u32 a);
void ps2_mmio_r128(ps2_reg128 *d, u32 a);
void ps2_mmio_w8(u32 a, u8 v);
void ps2_mmio_w16(u32 a, u16 v);
void ps2_mmio_w32(u32 a, u32 v);
void ps2_mmio_w64(u32 a, u64 v);
void ps2_mmio_w128(u32 a, const ps2_reg128 *v);

PS2_INLINE u8 ps2_r8(u32 a) {
    u8 *p = ps2_pt[a >> PS2_PAGE_BITS];
    if (PS2_LIKELY(p != NULL)) return p[a & PS2_PAGE_MASK];
    return ps2_mmio_r8(a);
}
PS2_INLINE u16 ps2_r16(u32 a) {
    u8 *p = ps2_pt[a >> PS2_PAGE_BITS];
    if (PS2_LIKELY(p != NULL)) { u16 v; memcpy(&v, p + (a & PS2_PAGE_MASK), 2); return v; }
    return ps2_mmio_r16(a);
}
PS2_INLINE u32 ps2_r32(u32 a) {
    u8 *p = ps2_pt[a >> PS2_PAGE_BITS];
    if (PS2_LIKELY(p != NULL)) { u32 v; memcpy(&v, p + (a & PS2_PAGE_MASK), 4); return v; }
    return ps2_mmio_r32(a);
}
PS2_INLINE u64 ps2_r64(u32 a) {
    u8 *p = ps2_pt[a >> PS2_PAGE_BITS];
    if (PS2_LIKELY(p != NULL)) { u64 v; memcpy(&v, p + (a & PS2_PAGE_MASK), 8); return v; }
    return ps2_mmio_r64(a);
}
PS2_INLINE void ps2_r128(ps2_reg128 *d, u32 a) {
    a &= ~15u;
    u8 *p = ps2_pt[a >> PS2_PAGE_BITS];
    if (PS2_LIKELY(p != NULL)) { memcpy(d, p + (a & PS2_PAGE_MASK), 16); return; }
    ps2_mmio_r128(d, a);
}
extern u32 ps2_watch_addr, ps2_watch_last;
void ps2_watch_hit(u32 addr, u64 val, int width);

PS2_INLINE void ps2_watch_check(u32 a, u64 v, int w) {
#if PS2_DIAG
    if (PS2_UNLIKELY(ps2_watch_addr != 0)
        && (a & ~3u) >= ps2_watch_addr && (a & ~3u) <= ps2_watch_last)
        ps2_watch_hit(a, v, w);
#else
    (void)a; (void)v; (void)w;
#endif
}

PS2_INLINE void ps2_w8(u32 a, u8 v) {
    ps2_watch_check(a, v, 1);
    u8 *p = ps2_pt[a >> PS2_PAGE_BITS];
    if (PS2_LIKELY(p != NULL)) { p[a & PS2_PAGE_MASK] = v; return; }
    ps2_mmio_w8(a, v);
}
PS2_INLINE void ps2_w16(u32 a, u16 v) {
    ps2_watch_check(a, v, 2);
    u8 *p = ps2_pt[a >> PS2_PAGE_BITS];
    if (PS2_LIKELY(p != NULL)) { memcpy(p + (a & PS2_PAGE_MASK), &v, 2); return; }
    ps2_mmio_w16(a, v);
}
PS2_INLINE void ps2_w32(u32 a, u32 v) {
    ps2_watch_check(a, v, 4);
    u8 *p = ps2_pt[a >> PS2_PAGE_BITS];
    if (PS2_LIKELY(p != NULL)) { memcpy(p + (a & PS2_PAGE_MASK), &v, 4); return; }
    ps2_mmio_w32(a, v);
}
PS2_INLINE void ps2_w64(u32 a, u64 v) {
    ps2_watch_check(a, v, 8);
    u8 *p = ps2_pt[a >> PS2_PAGE_BITS];
    if (PS2_LIKELY(p != NULL)) { memcpy(p + (a & PS2_PAGE_MASK), &v, 8); return; }
    ps2_mmio_w64(a, v);
}
PS2_INLINE void ps2_w128(u32 a, const ps2_reg128 *v) {
    a &= ~15u;
    ps2_watch_check(a, v->ud[0], 16);
    u8 *p = ps2_pt[a >> PS2_PAGE_BITS];
    if (PS2_LIKELY(p != NULL)) { memcpy(p + (a & PS2_PAGE_MASK), v, 16); return; }
    ps2_mmio_w128(a, v);
}

PS2_INLINE u32 ps2_lwl(u32 a, u32 old) {
    static const u32 mask[4] = {0x00FFFFFFu, 0x0000FFFFu, 0x000000FFu, 0x00000000u};
    static const u8 shift[4] = {24, 16, 8, 0};
    u32 al = a & 3u;
    u32 w = ps2_r32(a & ~3u);
    return (old & mask[al]) | (w << shift[al]);
}
PS2_INLINE u32 ps2_lwr(u32 a, u32 old) {
    static const u32 mask[4] = {0x00000000u, 0xFF000000u, 0xFFFF0000u, 0xFFFFFF00u};
    static const u8 shift[4] = {0, 8, 16, 24};
    u32 al = a & 3u;
    u32 w = ps2_r32(a & ~3u);
    return (old & mask[al]) | (w >> shift[al]);
}
PS2_INLINE void ps2_swl(u32 a, u32 val) {
    static const u32 mask[4] = {0xFFFFFF00u, 0xFFFF0000u, 0xFF000000u, 0x00000000u};
    static const u8 shift[4] = {24, 16, 8, 0};
    u32 al = a & 3u, base = a & ~3u;
    u32 w = ps2_r32(base);
    ps2_w32(base, (w & mask[al]) | (val >> shift[al]));
}
PS2_INLINE void ps2_swr(u32 a, u32 val) {
    static const u32 mask[4] = {0x00000000u, 0x000000FFu, 0x0000FFFFu, 0x00FFFFFFu};
    static const u8 shift[4] = {0, 8, 16, 24};
    u32 al = a & 3u, base = a & ~3u;
    u32 w = ps2_r32(base);
    ps2_w32(base, (w & mask[al]) | (val << shift[al]));
}
PS2_INLINE u64 ps2_ldl(u32 a, u64 old) {
    u32 al = a & 7u;
    u64 d = ps2_r64(a & ~7u);
    u32 sh = (u32)(56 - al * 8);
    u64 m = (al == 7) ? 0ull : (~0ull >> ((al + 1) * 8));
    return (old & m) | (d << sh);
}
PS2_INLINE u64 ps2_ldr(u32 a, u64 old) {
    u32 al = a & 7u;
    u64 d = ps2_r64(a & ~7u);
    u32 sh = (u32)(al * 8);
    u64 m = (al == 0) ? 0ull : (~0ull << ((8 - al) * 8));
    return (old & m) | (d >> sh);
}
PS2_INLINE void ps2_sdl(u32 a, u64 val) {
    u32 al = a & 7u, base = a & ~7u;
    u64 d = ps2_r64(base);
    u32 sh = (u32)(56 - al * 8);
    u64 m = (al == 7) ? 0ull : (~0ull << ((al + 1) * 8));
    ps2_w64(base, (d & m) | (val >> sh));
}
PS2_INLINE void ps2_sdr(u32 a, u64 val) {
    u32 al = a & 7u, base = a & ~7u;
    u64 d = ps2_r64(base);
    u32 sh = (u32)(al * 8);
    u64 m = (al == 0) ? 0ull : (~0ull >> ((8 - al) * 8));
    ps2_w64(base, (d & m) | (val << sh));
}

PS2_INLINE u32 ps2_plzcw(u32 x) {
    if (x & 0x80000000u) x = ~x;
    if (x == 0) return 31u;
    return (u32)__builtin_clz(x) - 1u;
}
void ps2_div(ps2_reg128 *lo, ps2_reg128 *hi, int pipe, s32 a, s32 b);
void ps2_divu(ps2_reg128 *lo, ps2_reg128 *hi, int pipe, u32 a, u32 b);

u32  ps2_cop0_read(ps2_ctx *ctx, int reg);
void ps2_cop0_write(ps2_ctx *ctx, int reg, u32 val);
void ps2_eret(ps2_ctx *ctx);
void ps2_tlb_op(ps2_ctx *ctx, int op);
void ps2_syscall(ps2_ctx *ctx);
void ps2_trap(ps2_ctx *ctx, u32 insn);
void ps2_unimplemented(ps2_ctx *ctx, u32 pc, u32 insn);
void ps2_dispatch(ps2_ctx *ctx, u32 addr);
void ps2_unknown_target(ps2_ctx *ctx, u32 addr);

u32  ps2_cfc1(ps2_ctx *ctx, int reg);
void ps2_ctc1(ps2_ctx *ctx, int reg, u32 val);

#define PS2_FMAX_U 0x7F7FFFFFu

PS2_INLINE float ps2_ftz(float v) {
    u32 u;
    memcpy(&u, &v, 4);
    if ((u & 0x7F800000u) == 0) { u &= 0x80000000u; memcpy(&v, &u, 4); }
    return v;
}
PS2_INLINE float ps2_fclamp(double d) {
    u32 u;
    float f;
    if (d != d) {
        u = PS2_FMAX_U;
        memcpy(&f, &u, 4);
        return f;
    }
    f = (float)d;
    memcpy(&u, &f, 4);
    if ((u & 0x7F800000u) == 0x7F800000u) {
        u = (u & 0x80000000u) | PS2_FMAX_U;
        memcpy(&f, &u, 4);
        return f;
    }
    if ((u & 0x7F800000u) == 0) { u &= 0x80000000u; memcpy(&f, &u, 4); }
    return f;
}
PS2_INLINE float ps2_fadd(float a, float b) {
    return ps2_fclamp((double)ps2_ftz(a) + (double)ps2_ftz(b));
}
PS2_INLINE float ps2_fsub(float a, float b) {
    return ps2_fclamp((double)ps2_ftz(a) - (double)ps2_ftz(b));
}
PS2_INLINE float ps2_fmul(float a, float b) {
    return ps2_fclamp((double)ps2_ftz(a) * (double)ps2_ftz(b));
}
float ps2_fdiv(float a, float b);
float ps2_fsqrt(float a);
float ps2_frsqrt(float a, float b);
PS2_INLINE float ps2_fmax(float a, float b) { return a > b ? a : b; }
PS2_INLINE float ps2_fmin(float a, float b) { return a < b ? a : b; }
PS2_INLINE float ps2_vu_select(float a, float b, int minimum) {
    u32 x, y;
    memcpy(&x, &a, 4); memcpy(&y, &b, 4);
    u32 kx = x ^ ((x & 0x80000000u) ? 0xFFFFFFFFu : 0x80000000u);
    u32 ky = y ^ ((y & 0x80000000u) ? 0xFFFFFFFFu : 0x80000000u);
    return (minimum ? kx < ky : kx > ky) ? a : b;
}
PS2_INLINE s32 ps2_cvt_w_s(float a) {
    u32 u;
    memcpy(&u, &a, 4);
    if ((u & 0x7F800000u) == 0x7F800000u)
        return (u & 0x80000000u) ? (s32)0x80000000 : (s32)0x7FFFFFFF;
    if (a >= 2147483648.0f) return (s32)0x7FFFFFFF;
    if (a <= -2147483648.0f) return (s32)0x80000000;
    return (s32)a;
}

int  ps2_runtime_init(const char *elf_path, const char *iso_path);
void ps2_runtime_run(void);
void ps2_runtime_shutdown(void);
extern ps2_ctx ps2_cpu;

extern int ps2_verbose;
extern int ps2_diag_armed;
extern volatile int ps2_capture_request;
void ps2_capture_report(const char *why);
extern unsigned ps2_trace_show;

int  ps2_fn_known(u32 addr);
void ps2_text_bounds(u32 *lo, u32 *hi);
void ps2_unknown_report(void);
void ps2_request_exit(void);
void ps2_finish(const char *why);
unsigned ps2_vblank_budget(void);
int  ps2_exit_pending(void);

enum {
    PS2_PH_GFXQ,
    PS2_PH_VIF,
    PS2_PH_UNPACK,
    PS2_PH_VU,
    PS2_PH_GIF,
    PS2_PH_TRX,
    PS2_PH_PRIM,
    PS2_PH_TEX,
    PS2_PH_VKDRAW,
    PS2_PH_DMA,
    PS2_PH_PRESENT,
    PS2_PH_N
};
extern u64 ps2_phase_cycles[PS2_PH_N];
extern u64 ps2_phase_self[PS2_PH_N];
extern u64 ps2_phase_calls[PS2_PH_N];
extern int ps2_phase_on;
extern __thread u64 ps2_phase_child;
void ps2_phase_init(void);
void ps2_phase_report(u64 fields);
void ps2_sampler_start(void);
void ps2_sampler_report(unsigned top);
double ps2_wall_seconds(void);

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <x86intrin.h>
PS2_INLINE u64 ps2_tsc(void) { return (u64)__rdtsc(); }
#else
PS2_INLINE u64 ps2_tsc(void) { return 0; }
#endif

#define PS2_PHASE_BEGIN(p)                                                    \
    u64 ps2_ph_t0_##p = 0, ps2_ph_c0_##p = 0;                                 \
    if (ps2_phase_on) {                                                       \
        ps2_ph_c0_##p = ps2_phase_child;                                      \
        ps2_phase_child = 0;                                                  \
        ps2_ph_t0_##p = ps2_tsc();                                            \
    }
#define PS2_PHASE_END(p)                                                      \
    do { if (ps2_phase_on) {                                                  \
        u64 ps2_ph_d = ps2_tsc() - ps2_ph_t0_##p;                             \
        ps2_phase_cycles[p] += ps2_ph_d;                                      \
        ps2_phase_self[p] += ps2_ph_d - ps2_phase_child;                      \
        ps2_phase_calls[p]++;                                                 \
        ps2_phase_child = ps2_ph_c0_##p + ps2_ph_d;                           \
    } } while (0)

void ps2_log(const char *fmt, ...);
void ps2_fatal(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#include "ps2_mmi.h"
#include "ps2_vu0.h"

#endif
