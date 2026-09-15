#include "ps2_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <pthread.h>

ps2_ctx ps2_cpu;
const ps2_reg128 ps2_zero_q = {{0, 0}};

u8 *ps2_pt[PS2_PT_ENTRIES];
u8 *ps2_ram;
u8 *ps2_spr;
u8 *ps2_iop_ram;
static u8 *ps2_vu0_mem;
static u8 *ps2_vu0_micro;
static u8 *ps2_vu1_mem;
static u8 *ps2_vu1_micro;

#if PS2_TRACE_CALLS
u32 ps2_trace_ring[PS2_TRACE_RING];
u32 ps2_trace_pos;
#endif

static FILE *ps2_logfp;

void ps2_log(const char *fmt, ...) {
    char line[1024];
    int n;
    va_list ap;
    if (!ps2_logfp) ps2_logfp = stderr;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 3) n = (int)sizeof(line) - 3;
    line[n] = '\n';
    line[n + 1] = 0;
    fwrite(line, 1, (size_t)n + 1, ps2_logfp);
    fflush(ps2_logfp);
}

u32 ps2_watch_addr, ps2_watch_last;
static u64 ps2_watch_hits;

void ps2_watch_hit(u32 addr, u64 val, int width) {
    u64 n = ++ps2_watch_hits;
    if (n > 4000) return;
    ps2_log("watch: %08X <- %0*llX (%d bytes)", addr, width * 2,
            (unsigned long long)val, width);
#if PS2_TRACE_CALLS
    {
        unsigned i;
        char line[256];
        int k = 0;
        for (i = 6; i > 0 && k < 200; i--) {
            u32 a = ps2_trace_ring[(ps2_trace_pos - i) & (PS2_TRACE_RING - 1)];
            const char *nm = ps2_symbol_name(a);
            k += snprintf(line + k, sizeof line - (size_t)k, " %08X%s%s", a,
                          nm ? ":" : "", nm ? nm : "");
        }
        ps2_log("   from%s", line);
    }
#endif
}

void ps2_dump_trace(const char *why) {
#if PS2_TRACE_CALLS
    unsigned n = ps2_trace_pos < PS2_TRACE_RING ? ps2_trace_pos : PS2_TRACE_RING;
    unsigned show = n < ps2_trace_show ? n : ps2_trace_show;
    unsigned i;
    u32 prev = 0;
    unsigned run = 0;
    ps2_log("---- call ring (%s), most recent last, %u of %u entries ----",
            why, show, n);
    for (i = show; i > 0; i--) {
        u32 a = ps2_trace_ring[(ps2_trace_pos - i) & (PS2_TRACE_RING - 1)];
        if (a == prev) { run++; continue; }
        if (run) ps2_log("        (repeated %u more)", run);
        run = 0;
        prev = a;
        {
            const char *nm = ps2_symbol_name(a);
            if (nm) ps2_log("   %08X  %s", a, nm);
            else    ps2_log("   %08X", a);
        }
    }
    if (run) ps2_log("        (repeated %u more)", run);
#else
    (void)why;
    ps2_log("call ring disabled at build time");
#endif
}

void ps2_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    ps2_dump_trace("fatal");
    exit(1);
}

void ps2_mem_map(u32 vaddr, u32 size, u8 *host) {
    u32 i;
    for (i = 0; i < size; i += PS2_PAGE_SIZE)
        ps2_pt[(vaddr + i) >> PS2_PAGE_BITS] = host + i;
}

static void map_ram_mirrors(void) {
    static const u32 bases[] = {
        0x00000000u,
        0x20000000u,
        0x30000000u,
        0x80000000u,
        0xA0000000u,
    };
    unsigned b;
    for (b = 0; b < sizeof(bases) / sizeof(bases[0]); b++)
        ps2_mem_map(bases[b], PS2_RAM_SIZE, ps2_ram);
}

void ps2_mem_init(void) {
    ps2_ram = (u8 *)calloc(1, PS2_RAM_SIZE + 16u);
    ps2_spr = (u8 *)calloc(1, PS2_SPR_SIZE + 16u);
    ps2_iop_ram = (u8 *)calloc(1, PS2_IOP_RAM_SIZE + 16u);
    ps2_vu0_mem = (u8 *)calloc(1, PS2_VU0_MEM_SIZE + 16u);
    ps2_vu0_micro = (u8 *)calloc(1, PS2_VU0_MICRO_SIZE + 16u);
    ps2_vu1_mem = (u8 *)calloc(1, PS2_VU1_MEM_SIZE + 16u);
    ps2_vu1_micro = (u8 *)calloc(1, PS2_VU1_MICRO_SIZE + 16u);
    if (!ps2_ram || !ps2_spr || !ps2_iop_ram || !ps2_vu0_mem || !ps2_vu1_mem || !ps2_vu0_micro || !ps2_vu1_micro)
        ps2_fatal("out of memory allocating guest address space");

    map_ram_mirrors();
    ps2_mem_map(PS2_SPR_BASE, PS2_SPR_SIZE, ps2_spr);
    ps2_mem_map(PS2_IOP_RAM_BASE, PS2_IOP_RAM_SIZE, ps2_iop_ram);
    ps2_mem_map(0x11000000u, PS2_VU0_MICRO_SIZE, ps2_vu0_micro);
    ps2_mem_map(0x11004000u, PS2_VU0_MEM_SIZE, ps2_vu0_mem);
    ps2_mem_map(0x11008000u, PS2_VU1_MICRO_SIZE, ps2_vu1_micro);
    ps2_mem_map(0x1100C000u, PS2_VU1_MEM_SIZE, ps2_vu1_mem);

    ps2_cpu.vu0.mem = ps2_vu0_mem;
    ps2_cpu.vu0.micro = ps2_vu0_micro;
    ps2_cpu.vu0.mem_size = PS2_VU0_MEM_SIZE;
    ps2_cpu.vu0.micro_size = PS2_VU0_MICRO_SIZE;
    ps2_cpu.vu0.vf[0].f[0] = 0.0f;
    ps2_cpu.vu0.vf[0].f[1] = 0.0f;
    ps2_cpu.vu0.vf[0].f[2] = 0.0f;
    ps2_cpu.vu0.vf[0].f[3] = 1.0f;
}

u8 *ps2_vu1_memory(void) { return ps2_vu1_mem; }
u8 *ps2_vu1_microcode(void) { return ps2_vu1_micro; }

#define PS2_TEXT_LO 0x00100000u
static ps2_fn *ps2_fn_index;
static u32 ps2_text_lo, ps2_text_hi;

void ps2_build_dispatch(void) {
    unsigned i;
    u32 lo = 0xFFFFFFFFu, hi = 0;
    for (i = 0; i < ps2_func_count; i++) {
        if (ps2_func_table[i].addr < lo) lo = ps2_func_table[i].addr;
        if (ps2_func_table[i].addr > hi) hi = ps2_func_table[i].addr;
    }
    ps2_text_lo = lo;
    ps2_text_hi = hi + 4;
    ps2_fn_index = (ps2_fn *)calloc((ps2_text_hi - ps2_text_lo) / 4 + 1,
                                    sizeof(ps2_fn));
    if (!ps2_fn_index) ps2_fatal("out of memory building dispatch table");
    for (i = 0; i < ps2_func_count; i++)
        ps2_fn_index[(ps2_func_table[i].addr - ps2_text_lo) >> 2] =
            ps2_func_table[i].fn;
    ps2_log("dispatch: %u functions, index %08X..%08X (%u slots)",
            ps2_func_count, ps2_text_lo, ps2_text_hi,
            (ps2_text_hi - ps2_text_lo) / 4);
}

int ps2_fn_known(u32 addr) {
    u32 off = addr - ps2_text_lo;
    if (off >= (ps2_text_hi - ps2_text_lo)) return 0;
    return ps2_fn_index[off >> 2] != NULL;
}

void ps2_text_bounds(u32 *lo, u32 *hi) {
    if (lo) *lo = ps2_text_lo;
    if (hi) *hi = ps2_text_hi;
}

#define UNKNOWN_SEEN_MAX 128
static u32 unknown_seen[UNKNOWN_SEEN_MAX];
static unsigned unknown_seen_n;
static u64 unknown_hits;

void ps2_unknown_target(ps2_ctx *ctx, u32 addr) {
    unsigned i;
    unknown_hits++;
    if (PS2_ENV("PS2_STRICT_BRANCH"))
        ps2_fatal("indirect branch to %08X, which no recompiled function "
                  "claims", addr);
    for (i = 0; i < unknown_seen_n; i++)
        if (unknown_seen[i] == addr) return;
    if (unknown_seen_n < UNKNOWN_SEEN_MAX) unknown_seen[unknown_seen_n++] = addr;
    ps2_log("");
    ps2_log("==== indirect branch to %08X, which no recompiled function "
            "claims ====", addr);
    ps2_log("The call is being skipped and control returned to the caller; "
            "whatever that function does will not happen.");
    if (!(addr & 3u) && addr >= ps2_text_lo && addr < ps2_text_hi)
        ps2_log("Possible recovery gap inside .text; inspect the target before adding a seed.");
    else
        ps2_log("Invalid code address (outside .text or misaligned); investigate pointer corruption, not function seeds.");
    if (ctx) {
        ps2_log("branch context: ra=%08X sp=%08X gp=%08X", ctx->r[31].uw[0],
                ctx->r[29].uw[0], ctx->r[28].uw[0]);
        for (unsigned r=2;r<28;r+=4)
            ps2_log("  r%02u=%08X r%02u=%08X r%02u=%08X r%02u=%08X",
                    r,ctx->r[r].uw[0],r+1,ctx->r[r+1].uw[0],
                    r+2,ctx->r[r+2].uw[0],r+3,ctx->r[r+3].uw[0]);
    }
    ps2_dump_trace("unrecovered function");
    ps2_log("");
}

void ps2_unknown_report(void) {
    unsigned i;
    if (!unknown_hits) return;
    ps2_log("unrecovered: %llu indirect branches into %u distinct addresses "
            "no recompiled function claims",
            (unsigned long long)unknown_hits, unknown_seen_n);
    for (i = 0; i < unknown_seen_n; i++)
        ps2_log("   %08X", unknown_seen[i]);
}

void ps2_dispatch(ps2_ctx *ctx, u32 addr) {
    u32 off = addr - ps2_text_lo;
    if (PS2_LIKELY(off < (ps2_text_hi - ps2_text_lo))) {
        ps2_fn f = ps2_fn_index[off >> 2];
        if (PS2_LIKELY(f != NULL)) {
            PS2_TAIL return f(ctx);
        }
    }
    ps2_unknown_target(ctx, addr);
}

void ps2_div(ps2_reg128 *lo, ps2_reg128 *hi, int pipe, s32 a, s32 b) {
    s32 q, r;
    if (b == 0) {
        q = (a < 0) ? 1 : -1;
        r = a;
    } else if ((u32)a == 0x80000000u && b == -1) {
        q = (s32)0x80000000;
        r = 0;
    } else {
        q = a / b;
        r = a % b;
    }
    lo->sd[pipe] = q;
    hi->sd[pipe] = r;
}

void ps2_divu(ps2_reg128 *lo, ps2_reg128 *hi, int pipe, u32 a, u32 b) {
    u32 q, r;
    if (b == 0) { q = 0xFFFFFFFFu; r = a; }
    else { q = a / b; r = a % b; }
    lo->sd[pipe] = (s32)q;
    hi->sd[pipe] = (s32)r;
}

u32 ps2_cop0_read(ps2_ctx *ctx, int reg) {
    switch (reg) {
    case 9:  return ctx->cop0[9];
    case 15: return 0x00002E20u;
    default: return ctx->cop0[reg & 31];
    }
}

void ps2_cop0_write(ps2_ctx *ctx, int reg, u32 val) {
    ctx->cop0[reg & 31] = val;
}

void ps2_eret(ps2_ctx *ctx) {
    ctx->cop0[12] &= ~0x2u;
}

void ps2_tlb_op(ps2_ctx *ctx, int op) {
    (void)ctx;
    (void)op;
}

void ps2_trap(ps2_ctx *ctx, u32 insn) {
    ps2_log("trap at pc=%08X insn=%08X", ctx->pc, insn);
    ps2_dump_trace("trap");
}

void ps2_unimplemented(ps2_ctx *ctx, u32 pc, u32 insn) {
    (void)ctx;
    ps2_fatal("unimplemented instruction %08X at %08X", insn, pc);
}

u32 ps2_cfc1(ps2_ctx *ctx, int reg) {
    if (reg == 0) return 0x00002E00u;
    return (ctx->fcr31 & ~0x00800000u) | (ctx->fcc ? 0x00800000u : 0u);
}

void ps2_ctc1(ps2_ctx *ctx, int reg, u32 val) {
    if (reg == 31) {
        ctx->fcr31 = val;
        ctx->fcc = (val >> 23) & 1u;
    }
}

float ps2_fdiv(float a, float b) {
    u32 ub, ua, r;
    float f;
    memcpy(&ub, &b, 4);
    if ((ub & 0x7FFFFFFFu) == 0) {
        memcpy(&ua, &a, 4);
        r = ((ua ^ ub) & 0x80000000u) | PS2_FMAX_U;
        memcpy(&f, &r, 4);
        return f;
    }
    return ps2_fclamp((double)ps2_ftz(a) / (double)ps2_ftz(b));
}

float ps2_fsqrt(float a) {
    float v = ps2_ftz(a);
    if (v < 0.0f) v = -v;
    return ps2_fclamp(sqrt((double)v));
}

float ps2_frsqrt(float a, float b) {
    float v = ps2_ftz(b);
    if (v < 0.0f) v = -v;
    if (v == 0.0f) {
        u32 ua, r;
        float f;
        memcpy(&ua, &a, 4);
        r = (ua & 0x80000000u) | PS2_FMAX_U;
        memcpy(&f, &r, 4);
        return f;
    }
    return ps2_fclamp((double)ps2_ftz(a) / sqrt((double)v));
}

void ps2_vu0_div(ps2_ctx *ctx, int fs, int fsf, int ft, int ftf) {
    ctx->vu0.q = ps2_fdiv(ctx->vu0.vf[fs].f[fsf], ctx->vu0.vf[ft].f[ftf]);
}
void ps2_vu0_sqrt(ps2_ctx *ctx, int ft, int ftf) {
    ctx->vu0.q = ps2_fsqrt(ctx->vu0.vf[ft].f[ftf]);
}
void ps2_vu0_rsqrt(ps2_ctx *ctx, int fs, int fsf, int ft, int ftf) {
    ctx->vu0.q = ps2_frsqrt(ctx->vu0.vf[fs].f[fsf], ctx->vu0.vf[ft].f[ftf]);
}

static float r_as_float(u32 r) {
    u32 bits = 0x3F800000u | (r & 0x007FFFFFu);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
void ps2_vu0_rnext(ps2_ctx *ctx, int dest, int ft) {
    u32 r = ctx->vu0.r;
    u32 bit = ((r >> 4) & 1u) ^ ((r >> 22) & 1u);
    float v[4];
    int i;
    r = ((r << 1) | bit) & 0x007FFFFFu;
    ctx->vu0.r = r;
    for (i = 0; i < 4; i++) v[i] = r_as_float(r);
    ps2_vu_store(&ctx->vu0, ft, dest, v);
}
void ps2_vu0_rget(ps2_ctx *ctx, int dest, int ft) {
    float v[4];
    int i;
    for (i = 0; i < 4; i++) v[i] = r_as_float(ctx->vu0.r);
    ps2_vu_store(&ctx->vu0, ft, dest, v);
}
void ps2_vu0_rinit(ps2_ctx *ctx, int fs, int fsf) {
    ctx->vu0.r = ctx->vu0.vf[fs].u[fsf] & 0x007FFFFFu;
}
void ps2_vu0_rxor(ps2_ctx *ctx, int fs, int fsf) {
    ctx->vu0.r ^= ctx->vu0.vf[fs].u[fsf] & 0x007FFFFFu;
}

static u32 vu_fbrst;
u32 ps2_vu0_cfc(ps2_ctx *ctx, int reg) {
    ps2_vu *vu = &ctx->vu0;
    u32 v;
    if (reg < 16) return vu->vi[reg];
    switch (reg) {
    case 16: return vu->status;
    case 17: return vu->mac;
    case 18: return vu->clip;
    case 20: return vu->r;
    case 21: memcpy(&v, &vu->i, 4); return v;
    case 22: memcpy(&v, &vu->q, 4); return v;
    case 23: memcpy(&v, &vu->p, 4); return v;
    case 27: return vu->cmsar;
    case 28: return vu_fbrst;
    case 29: return 0;
    default: return 0;
    }
}

void ps2_vu0_ctc(ps2_ctx *ctx, int reg, u32 val) {
    ps2_vu *vu = &ctx->vu0;
    if (reg < 16) { if (reg) vu->vi[reg] = (u16)val; return; }
    switch (reg) {
    case 16:
        ps2_vu0_advance(ctx, 5);
        vu->status = (vu->status & 0x3Fu) | (val & 0xFC0u);
        break;
    case 17: break;
    case 18: vu->clip = val; break;
    case 20: vu->r = val & 0x007FFFFFu; break;
    case 21: memcpy(&vu->i, &val, 4); break;
    case 22: memcpy(&vu->q, &val, 4); break;
    case 23: memcpy(&vu->p, &val, 4); break;
    case 27: vu->cmsar = val & 0xFFFFu; break;
    case 28: {
        void ps2_vu_control_stop(ps2_ctx *, u32);
        vu_fbrst = val & 0x0C0Cu;
        ps2_vu_control_stop(ctx, val);
        if (val & 2u) vu_fbrst &= ~0xFFu;
        if (val & 0x200u) vu_fbrst &= ~0xFF00u;
        break;
    }
    default: break;
    }
}

void ps2_get_str(u32 addr, char *dst, size_t cap) {
    size_t i = 0;
    if (!cap) return;
    if (!addr) { dst[0] = 0; return; }
    for (; i + 1 < cap; i++) {
        u8 c = ps2_r8(addr + (u32)i);
        dst[i] = (char)c;
        if (!c) return;
    }
    dst[i] = 0;
}

void ps2_put_mem(u32 addr, const void *src, size_t n) {
    const u8 *s = (const u8 *)src;
    u8 *p = ps2_pt[addr >> PS2_PAGE_BITS];
    if (p && ((addr & PS2_PAGE_MASK) + n) <= PS2_PAGE_SIZE) {
        memcpy(p + (addr & PS2_PAGE_MASK), s, n);
        return;
    }
    for (size_t i = 0; i < n; i++) ps2_w8(addr + (u32)i, s[i]);
}

void ps2_get_mem(void *dst, u32 addr, size_t n) {
    u8 *d = (u8 *)dst;
    u8 *p = ps2_pt[addr >> PS2_PAGE_BITS];
    if (p && ((addr & PS2_PAGE_MASK) + n) <= PS2_PAGE_SIZE) {
        memcpy(d, p + (addr & PS2_PAGE_MASK), n);
        return;
    }
    for (size_t i = 0; i < n; i++) d[i] = ps2_r8(addr + (u32)i);
}

int ps2_verbose;
unsigned ps2_trace_show = 64;
static volatile int ps2_exit_requested;
void ps2_request_exit(void) {
    __atomic_store_n(&ps2_exit_requested, 1, __ATOMIC_RELEASE);
}
int  ps2_exit_pending(void) {
    return __atomic_load_n(&ps2_exit_requested, __ATOMIC_ACQUIRE);
}

const char *ps2_symbol_name(u32 addr) {
    unsigned lo = 0, hi = ps2_symbol_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (ps2_symbols[mid].addr == addr) return ps2_symbols[mid].name;
        if (ps2_symbols[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

u32 *ps2_prof;
u32 ps2_prof_base, ps2_prof_size;

void ps2_prof_enable(u32 base, u32 size) {
    ps2_prof_base = base;
    ps2_prof_size = size;
    ps2_prof = (u32 *)calloc(size / 4u + 1u, sizeof(u32));
    if (!ps2_prof) ps2_log("prof: out of memory, profiling disabled");
}

void ps2_prof_report(unsigned top) {
    u32 *idx;
    unsigned n = 0, i, j;
    if (!ps2_prof) return;
    idx = (u32 *)malloc(sizeof(u32) * top);
    if (!idx) return;
    for (i = 0; i < ps2_prof_size / 4u; i++) {
        if (!ps2_prof[i]) continue;
        for (j = 0; j < n; j++)
            if (ps2_prof[i] > ps2_prof[idx[j]]) break;
        if (j == top) continue;
        if (n < top) n++;
        for (unsigned k = n - 1; k > j; k--) idx[k] = idx[k - 1];
        idx[j] = i;
    }
    ps2_log("---- entry histogram, top %u ----", n);
    for (i = 0; i < n; i++) {
        u32 a = ps2_prof_base + idx[i] * 4u;
        const char *nm = ps2_symbol_name(a);
        ps2_log("   %10u  %08X  %s", ps2_prof[idx[i]], a, nm ? nm : "");
    }
    free(idx);
}

u64 ps2_phase_cycles[PS2_PH_N];
u64 ps2_phase_self[PS2_PH_N];
u64 ps2_phase_calls[PS2_PH_N];
int ps2_phase_on;
__thread u64 ps2_phase_child;
static u64 ps2_phase_t0;
static double ps2_phase_wall0;
static double ps2_phase_hz;

void ps2_phase_init(void) {
    const char *e = getenv("PS2_PROFILE_PHASES");
    ps2_phase_on = e && *e && *e != '0';
    memset(ps2_phase_cycles, 0, sizeof ps2_phase_cycles);
    memset(ps2_phase_self, 0, sizeof ps2_phase_self);
    memset(ps2_phase_calls, 0, sizeof ps2_phase_calls);
    ps2_phase_t0 = ps2_tsc();
    ps2_phase_wall0 = ps2_wall_seconds();
}

static double phase_ms(u64 cycles, double secs, u64 total_tsc) {
    if (!total_tsc || secs <= 0.0) return 0.0;
    return (double)cycles / (double)total_tsc * secs * 1000.0;
}

void ps2_phase_report(u64 fields) {
    static const char *nm[PS2_PH_N] = {
        "gfxq: record dispatch", "VIF walker",
        "  of VIF: UNPACK", "  of VIF: VU1 microprograms",
        "GIF packets",
        "  of GIF: image transfer", "  of GIF: primitive assembly",
        "    of prim: texture", "    of prim: display list",
        "DMAC", "renderer: replay"
    };
    u64 now = ps2_tsc();
    u64 span = now - ps2_phase_t0;
    double secs = ps2_wall_seconds() - ps2_phase_wall0;
    int i;
    if (!ps2_phase_on) return;
    ps2_log("---- phase profile (%.1f s wall, %llu fields = %.1f fields/s) ----",
            secs, (unsigned long long)fields,
            secs > 0.0 ? (double)fields / secs : 0.0);
    ps2_log("   %-28s %10s %10s %12s %9s", "phase", "ms/field", "self",
            "calls/field", "self %");
    for (i = 0; i < PS2_PH_N; i++) {
        double ms = phase_ms(ps2_phase_cycles[i], secs, span);
        double self = phase_ms(ps2_phase_self[i], secs, span);
        if (!ps2_phase_calls[i]) continue;
        ps2_log("   %-28s %10.3f %10.3f %12.1f %8.1f%%", nm[i],
                fields ? ms / (double)fields : 0.0,
                fields ? self / (double)fields : 0.0,
                fields ? (double)ps2_phase_calls[i] / (double)fields : 0.0,
                span ? 100.0 * (double)ps2_phase_self[i] / (double)span : 0.0);
    }
    ps2_log("   (the first column includes everything nested inside the phase, "
            "`self` excludes it.");
    ps2_log("    gfxq record dispatch is the graphics worker's whole span and "
            "contains every phase");
    ps2_log("    below it; the VIF walker contains UNPACK and VU1; VU1 contains "
            "a whole GIF packet");
    ps2_log("    every XGKICK; GIF contains image transfer and primitive "
            "assembly, and primitive");
    ps2_log("    assembly contains texture and display list.  gfxq `self` is "
            "the ring decode and");
    ps2_log("    dispatch alone, VIF `self` the command walk with no UNPACK, "
            "VU1 or GIF in it.");
    ps2_log("    A VU0 microprogram entered by COP2 CALLMS is counted in the "
            "VU1 row with no VIF");
    ps2_log("    parent, so that row can exceed the VIF walker if this title "
            "drives VU0.)");
}

double ps2_wall_seconds(void) {
    static u64 start;
    struct timespec ts;
    u64 now;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
    if (!start) { start = now; return 0.0; }
    return (double)(now - start) / 1e9;
}

#define PS2_SAMP_N 4096
static struct { u32 addr; u64 n; } samp[PS2_SAMP_N];
static unsigned samp_n;
static u64 samp_total;
static volatile int samp_running;
static pthread_t samp_thread;

static void samp_record(u32 addr) {
    unsigned i;
    for (i = 0; i < samp_n; i++)
        if (samp[i].addr == addr) { samp[i].n++; samp_total++; return; }
    if (samp_n >= PS2_SAMP_N) return;
    samp[samp_n].addr = addr;
    samp[samp_n].n = 1;
    samp_n++;
    samp_total++;
}

static void *samp_main(void *unused) {
    (void)unused;
    while (samp_running) {
        struct timespec ts = { 0, 1000000 };
        u32 pos, addr;
        nanosleep(&ts, NULL);
        pos = __atomic_load_n(&ps2_trace_pos, __ATOMIC_RELAXED);
        addr = ps2_trace_ring[(pos - 1u) & (PS2_TRACE_RING - 1u)];
        if (addr) samp_record(addr);
    }
    return NULL;
}

void ps2_sampler_start(void) {
    const char *e = getenv("PS2_PROFILE_GUEST");
    if (!e || !*e || *e == '0') return;
#if !PS2_TRACE_CALLS
    ps2_log("guest sampler: the call ring is disabled at build time");
    return;
#else
    samp_running = 1;
    if (pthread_create(&samp_thread, NULL, samp_main, NULL) != 0)
        samp_running = 0;
#endif
}

void ps2_sampler_report(unsigned top) {
    unsigned i, j, n = 0;
    u32 idx[64];
    if (!samp_total) return;
    samp_running = 0;
    if (top > 64) top = 64;
    for (i = 0; i < samp_n; i++) {
        for (j = 0; j < n; j++) if (samp[i].n > samp[idx[j]].n) break;
        if (j == top) continue;
        if (n < top) n++;
        for (unsigned k = n - 1; k > j; k--) idx[k] = idx[k - 1];
        idx[j] = i;
    }
    ps2_log("---- guest sampling profile (%llu samples, %u distinct) ----",
            (unsigned long long)samp_total, samp_n);
    ps2_log("   Samples identify the last entered guest function, not the current PC; waits and callees may be attributed to it.");
    ps2_log("   %8s  %-10s %s", "% samples", "address", "symbol");
    for (i = 0; i < n; i++) {
        u32 a = samp[idx[i]].addr;
        const char *nm = ps2_symbol_name(a);
        ps2_log("   %7.2f%%  %08X   %s", 100.0 * (double)samp[idx[i]].n
                / (double)samp_total, a, nm ? nm : "");
    }
}
