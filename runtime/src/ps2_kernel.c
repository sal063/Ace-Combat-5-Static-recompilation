#include "ps2_runtime.h"
#include "ps2_hle.h"
#include "ps2_statecap.h"

int ps2_gs_present(void);
void ps2_gs_stats(u64 *prims, u64 *pixels, u64 *regs);
void ps2_gif_dump_arm(u32 packets);
void ps2_gs_census_reset(void);
unsigned ps2_vblank_budget(void);
void ps2_finish(const char *why);
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static _Thread_local HANDLE field_timer;
#endif

#define THS_RUN         0x01
#define THS_READY       0x02
#define THS_WAIT        0x04
#define THS_SUSPEND     0x08
#define THS_WAITSUSPEND 0x0c
#define THS_DORMANT     0x10

#define PS2_MAX_THREADS 256
#define PS2_MAX_SEMA    256
#define PS2_MAX_HANDLER 128

typedef struct {
    int used, status;
    u32 entry, stack, stack_size, gp, option, attr;
    int init_prio, cur_prio;
    int wakeup_count;
    int waiting_sema;
    u32 arg;
    int started, exited;
    int terminate_requested;
    pthread_t th;
    pthread_cond_t cv;
    ps2_ctx ctx;
} ps2_thread;

typedef struct {
    int used;
    int count, max_count, init_count, wait_threads;
    u32 generation;
    u32 attr, option;
} ps2_sema;

typedef struct {
    int used, cause;
    u32 handler, arg, gp;
    int next;
} ps2_handler;

static ps2_thread threads[PS2_MAX_THREADS];
static ps2_sema semas[PS2_MAX_SEMA];
static ps2_handler intc_h[PS2_MAX_HANDLER];
static ps2_handler dmac_h[PS2_MAX_HANDLER];

static u32 dmac_handler_mask;

static pthread_mutex_t ee_lock = PTHREAD_MUTEX_INITIALIZER;

#define TID_NONE (-1)
#define TID_INTR (-2)
static int current_tid = 0;

static __thread int self_tid = -1;
static __thread int in_intr = 0;
static u32 intc_enabled, dmac_enabled;
static u32 osd_param, osd_param2;
static u32 gs_imr_shadow = 0xFF00u;
static u32 vsync_flag_ptr, vsync_flag_val;
static u64 syscall_counts[300];
static u64 unhandled_syscalls[300], unhandled_out_of_range;
static void retire_thread_locked(int self);
static void check_thread_termination(void);
static int kernel_exit_requested;
static u32 main_stack_base = 0x01F00000u, main_stack_size = 0x100000u;
static u32 heap_base, heap_end = 0x01F00000u;

static ps2_ctx intr_ctx;
static u32 intr_stack_top;
static u32 sif_dma_id;
static u64 sif_dma_bytes;
static volatile u32 vblank_pending;
static volatile u64 vblank_delivered;
static volatile u64 vblank_generated;
static volatile u64 vblank_missed;
static pthread_t vblank_th;
static int vblank_running;

void ps2_gs_vblank(void);
void ps2_sif_set_reg(u32 reg, u32 val);
u32  ps2_sif_get_reg(u32 reg);
void ps2_timers_tick(void);
void ps2_intc_ack(int irq);
u32  ps2_intc_pending(void);
void ps2_pad_latch(void);

static int pick_next(int except) {
    int best = -1, bp = 0x7FFFFFFF;
    for (int i = 0; i < PS2_MAX_THREADS; i++) {
        if (!threads[i].used || i == except || threads[i].exited) continue;
        if (threads[i].status != THS_READY && threads[i].status != THS_RUN)
            continue;
        if (threads[i].cur_prio < bp) { bp = threads[i].cur_prio; best = i; }
    }
    return best;
}

static void deliver_vblank(void);

void ps2_kernel_run_dmac(ps2_ctx *ctx, int cause);
u64 ps2_kernel_vblank_count(void);

static void run_dmac_pass(u32 pend) {
    static u32 said;
    int c;
    memset(&intr_ctx.r[0], 0, sizeof(intr_ctx.r));
    intr_ctx.r[29].ud[0] = intr_stack_top;
    intr_ctx.cop0[12] = 0x70000000u;
    for (c = 0; c < 10; c++) {
        if (!(pend & (1u << c))) continue;
        if (!(said & (1u << c))) {
            int i, have = 0;
            for (i = 0; i < PS2_MAX_HANDLER; i++)
                if (dmac_h[i].used && dmac_h[i].cause == c) have++;
            said |= 1u << c;
            ps2_log("dmac: channel %d completed; handlers registered=%d, "
                    "EnableDmac(%d)=%s -> %s", c, have, c,
                    (dmac_enabled & (1u << c)) ? "yes" : "NO",
                    (have && (dmac_enabled & (1u << c))) ? "dispatching"
                                                         : "NOTHING TO RUN");
        }
        ps2_dmac_ack(c);
        ps2_kernel_run_dmac(&intr_ctx, c);
    }
}

static void run_dmac_completions(void) {
    u32 pend;
    int round;
    for (round = 0; round < 16 && (pend = ps2_dmac_pending()) != 0; round++)
        run_dmac_pass(pend);
}

u32 ps2_loop_ctr;

void ps2_loop_service(void) {
    u32 pend;
    check_thread_termination();
    if (in_intr) return;
    pend = ps2_dmac_pending() & dmac_enabled & dmac_handler_mask;
    if (!pend) return;
    {
        static u32 said;
        if (~said & pend) {
            said |= pend;
            ps2_log("preempt: a guest loop back edge reached the runtime with "
                    "DMAC status %03X pending -- delivering", pend);
        }
    }
    ps2_preempt();
}

static int run_pending_vblanks(int owner) {
    int did = 0;
    if (in_intr) return 0;
    if (current_tid != owner) return 0;
    while (__atomic_load_n(&vblank_pending, __ATOMIC_RELAXED)
           && !kernel_exit_requested) {
        __atomic_fetch_sub(&vblank_pending, 1, __ATOMIC_ACQ_REL);
        current_tid = TID_INTR;
        in_intr = 1;
        pthread_mutex_unlock(&ee_lock);
        deliver_vblank();
        pthread_mutex_lock(&ee_lock);
        in_intr = 0;
        current_tid = owner;
        did = 1;
    }
    return did;
}

static void yield_to_scheduler(int self) {
    for (;;) {
        int next;
        struct timespec ts;

        if (threads[self].terminate_requested) retire_thread_locked(self);
        if (kernel_exit_requested) return;
        if (current_tid == self) return;
        if (current_tid == TID_NONE && threads[self].status == THS_READY) {
            current_tid = self;
            return;
        }

        next = current_tid == TID_NONE ? pick_next(self) : -1;
        if (next >= 0) {
            current_tid = next;
            threads[next].status = THS_RUN;
            pthread_cond_signal(&threads[next].cv);
        } else if (current_tid == TID_NONE
                   && __atomic_load_n(&vblank_pending, __ATOMIC_RELAXED)) {
            if (run_pending_vblanks(TID_NONE)) continue;
        }

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 2000000;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&threads[self].cv, &ee_lock, &ts);
    }
}

static void block_self_locked(int status) {
    int self = self_tid;
    if (in_intr || self < 0) {
        static int said;
        if (!said) {
            said = 1;
            ps2_log("kernel: a blocking call was made from interrupt context "
                    "(status %02X); ignoring it", status);
        }
        return;
    }
    threads[self].status = status;
    if (current_tid == self) current_tid = TID_NONE;
    yield_to_scheduler(self);
    threads[self].status = THS_RUN;
    current_tid = self;
}

static void make_ready(int tid) {
    if (tid < 0 || tid >= PS2_MAX_THREADS || !threads[tid].used) return;
    if (threads[tid].status == THS_WAITSUSPEND) {
        threads[tid].status = THS_SUSPEND;
        return;
    }
    if (threads[tid].status == THS_WAIT) {
        threads[tid].status = THS_READY;
        pthread_cond_signal(&threads[tid].cv);
    }
}

static void release_token(int self) {
    int next;
    if (current_tid == self) current_tid = TID_NONE;
    if (current_tid != TID_NONE) return;
    next = pick_next(self);
    if (next < 0) return;
    current_tid = next;
    threads[next].status = THS_RUN;
    pthread_cond_signal(&threads[next].cv);
}

static void retire_thread_locked(int self) {
    ps2_thread *t = &threads[self];
    if (t->waiting_sema > 0 && semas[t->waiting_sema].wait_threads > 0)
        semas[t->waiting_sema].wait_threads--;
    t->waiting_sema = 0;
    t->status = THS_DORMANT;
    t->exited = 1;
    release_token(self);
    pthread_mutex_unlock(&ee_lock);
    pthread_exit(NULL);
}

static void check_thread_termination(void) {
    if (in_intr || self_tid <= 0) return;
    if (!__atomic_load_n(&threads[self_tid].terminate_requested, __ATOMIC_RELAXED)) return;
    pthread_mutex_lock(&ee_lock);
    retire_thread_locked(self_tid);
}

static void *thread_trampoline(void *arg) {
    ps2_thread *t = (ps2_thread *)arg;
    int self = (int)(t - threads);
    self_tid = self;
    pthread_mutex_lock(&ee_lock);
    while (current_tid != self && !kernel_exit_requested && !t->terminate_requested)
        pthread_cond_wait(&t->cv, &ee_lock);
    if (t->terminate_requested) retire_thread_locked(self);
    pthread_mutex_unlock(&ee_lock);
    if (kernel_exit_requested) return NULL;

    t->ctx.r[29].ud[0] = t->stack + t->stack_size - 16;
    t->ctx.r[28].ud[0] = t->gp;
    t->ctx.r[4].ud[0] = t->arg;
    t->ctx.r[31].ud[0] = 0;
    ps2_log("kernel: thread %d entering %08X (sp=%08X gp=%08X)",
            self, t->entry, (u32)t->ctx.r[29].ud[0], (u32)t->gp);
    ps2_dispatch(&t->ctx, t->entry);

    pthread_mutex_lock(&ee_lock);
    t->status = THS_DORMANT;
    t->exited = 1;
    release_token(self);
    pthread_mutex_unlock(&ee_lock);
    return NULL;
}

static u32 g32(u32 a) { return ps2_r32(a); }
static void s32w(u32 a, u32 v) { ps2_w32(a, v); }

#define A0 ((u32)ctx->r[4].ud[0])
#define A1 ((u32)ctx->r[5].ud[0])
#define A2 ((u32)ctx->r[6].ud[0])
#define A3 ((u32)ctx->r[7].ud[0])
#define RET(v) do { ctx->r[2].sd[0] = (s64)(s32)(v); } while (0)
#define RET64(v) do { ctx->r[2].ud[0] = (u64)(v); } while (0)

static int sys_create_thread(ps2_ctx *ctx) {
    u32 p = A0;
    int i;
    pthread_mutex_lock(&ee_lock);
    for (i = 1; i < PS2_MAX_THREADS; i++)
        if (!threads[i].used && (!threads[i].started || threads[i].exited)) break;
    if (i >= PS2_MAX_THREADS) { pthread_mutex_unlock(&ee_lock); return -1; }
    if (threads[i].started) pthread_join(threads[i].th, NULL);
    if (threads[i].entry) pthread_cond_destroy(&threads[i].cv);
    memset(&threads[i], 0, sizeof(threads[i]));
    threads[i].used = 1;
    threads[i].status = THS_DORMANT;
    threads[i].entry      = g32(p + 4);
    threads[i].stack      = g32(p + 8);
    threads[i].stack_size = g32(p + 12);
    threads[i].gp         = g32(p + 16);
    threads[i].init_prio  = (int)g32(p + 20);
    threads[i].cur_prio   = threads[i].init_prio;
    threads[i].attr       = g32(p + 28);
    threads[i].option     = g32(p + 32);
    pthread_cond_init(&threads[i].cv, NULL);
    ps2_log("kernel: CreateThread(param=%08X) -> id %d "
            "entry=%08X stack=%08X size=%u gp=%08X prio=%d",
            p, i, threads[i].entry, threads[i].stack, threads[i].stack_size,
            threads[i].gp, threads[i].init_prio);
    pthread_mutex_unlock(&ee_lock);
    return i;
}

static void sys_start_thread(ps2_ctx *ctx) {
    int id = (int)A0;
    pthread_mutex_lock(&ee_lock);
    if (id <= 0 || id >= PS2_MAX_THREADS || !threads[id].used ||
        threads[id].status != THS_DORMANT || (threads[id].started && !threads[id].exited)) {
        pthread_mutex_unlock(&ee_lock); RET(-1); return;
    }
    if (threads[id].started) pthread_join(threads[id].th, NULL);
    threads[id].started = threads[id].exited = 0;
    __atomic_store_n(&threads[id].terminate_requested, 0, __ATOMIC_RELAXED);
    memset(&threads[id].ctx, 0, sizeof(threads[id].ctx));
    threads[id].ctx.vu0.mem = ps2_cpu.vu0.mem;
    threads[id].ctx.vu0.micro = ps2_cpu.vu0.micro;
    threads[id].ctx.vu0.mem_size = ps2_cpu.vu0.mem_size;
    threads[id].ctx.vu0.micro_size = ps2_cpu.vu0.micro_size;
    threads[id].ctx.vu0.vf[0].f[3] = 1.0f;
    threads[id].arg = A1;
    threads[id].status = THS_READY;
    if (pthread_create(&threads[id].th, NULL, thread_trampoline, &threads[id]) != 0) {
        threads[id].status = THS_DORMANT;
        pthread_mutex_unlock(&ee_lock); RET(-1); return;
    }
    threads[id].started = 1;
    pthread_mutex_unlock(&ee_lock);
    RET(id);
}

static void sys_refer_thread_status(ps2_ctx *ctx) {
    int id = (int)A0;
    u32 p = A1;
    ps2_thread *t;
    pthread_mutex_lock(&ee_lock);
    if (id == 0) id = self_tid;
    if (id < 0 || id >= PS2_MAX_THREADS || !threads[id].used) {
        pthread_mutex_unlock(&ee_lock); RET(-1); return;
    }
    t = &threads[id];
    if (p) {
        s32w(p + 0, (u32)t->status);
        s32w(p + 4, t->entry);
        s32w(p + 8, t->stack);
        s32w(p + 12, t->stack_size);
        s32w(p + 16, t->gp);
        s32w(p + 20, (u32)t->init_prio);
        s32w(p + 24, (u32)t->cur_prio);
        s32w(p + 28, t->attr);
        s32w(p + 32, t->option);
        s32w(p + 36, 0);
        s32w(p + 40, 0);
        s32w(p + 44, (u32)t->wakeup_count);
    }
    RET(t->status);
    pthread_mutex_unlock(&ee_lock);
}

static int sys_create_sema(ps2_ctx *ctx) {
    u32 p = A0;
    int i;
    for (i = 1; i < PS2_MAX_SEMA; i++) if (!semas[i].used) break;
    if (i >= PS2_MAX_SEMA) return -1;
    semas[i].generation++;
    semas[i].used = 1;
    semas[i].max_count  = (int)g32(p + 4);
    semas[i].init_count = (int)g32(p + 8);
    semas[i].count      = semas[i].init_count;
    semas[i].wait_threads = 0;
    semas[i].attr   = g32(p + 16);
    semas[i].option = g32(p + 20);
    ps2_log("kernel: CreateSema(param=%08X) -> id %d  raw[+0]=%d [+4]=%d "
            "[+8]=%d [+12]=%d [+16]=%08X [+20]=%08X",
            p, i, (int)g32(p + 0), (int)g32(p + 4), (int)g32(p + 8),
            (int)g32(p + 12), g32(p + 16), g32(p + 20));
    return i;
}

static int trace_sema(void) {
    static int t = -1;
    if (t < 0) { const char *e = getenv("PS2_TRACE_SEMA"); t = (e && *e && *e != '0'); }
    return t;
}

static void sys_wait_sema(ps2_ctx *ctx) {
    int id = (int)A0;
    int self = self_tid;
    pthread_mutex_lock(&ee_lock);
    if (id <= 0 || id >= PS2_MAX_SEMA || !semas[id].used) {
        pthread_mutex_unlock(&ee_lock); RET(-1); return;
    }
    u32 generation = semas[id].generation;
    for (;;) {
        if (!semas[id].used || semas[id].generation != generation) {
            pthread_mutex_unlock(&ee_lock); RET(-1); return;
        }
        if (semas[id].count > 0) { semas[id].count--; break; }
        if (in_intr || self < 0) {
            pthread_mutex_unlock(&ee_lock);
            RET(-1);
            return;
        }
        if (kernel_exit_requested) {
            pthread_mutex_unlock(&ee_lock);
            RET(-1);
            return;
        }
        if (trace_sema())
            ps2_log("sema: thread %d BLOCKS on sema %d", self, id);
        semas[id].wait_threads++;
        threads[self].waiting_sema = id;
        block_self_locked(THS_WAIT);
        threads[self].waiting_sema = 0;
        if (semas[id].generation == generation && semas[id].wait_threads > 0) semas[id].wait_threads--;
        if (!semas[id].used) { pthread_mutex_unlock(&ee_lock); RET(-1); return; }
    }
    pthread_mutex_unlock(&ee_lock);
    RET(id);
}

static void sys_signal_sema(ps2_ctx *ctx) {
    int id = (int)A0;
    pthread_mutex_lock(&ee_lock);
    if (id <= 0 || id >= PS2_MAX_SEMA || !semas[id].used) {
        pthread_mutex_unlock(&ee_lock); RET(-1); return;
    }
    if (trace_sema())
        ps2_log("sema: thread %d signals sema %d (count %d)",
                self_tid, id, semas[id].count);
    if (semas[id].count < semas[id].max_count) semas[id].count++;
    {
        int best = -1, bp = 0x7FFFFFFF;
        for (int i = 0; i < PS2_MAX_THREADS; i++) {
            if (!threads[i].used || threads[i].waiting_sema != id) continue;
            if (threads[i].status != THS_WAIT
                && threads[i].status != THS_WAITSUSPEND) continue;
            if (threads[i].cur_prio < bp) { bp = threads[i].cur_prio; best = i; }
        }
        if (trace_sema())
            ps2_log("sema:   sema %d -> %s%d", id,
                    best >= 0 ? "wakes thread " : "no waiter (", best);
        if (best >= 0) make_ready(best);
    }
    pthread_mutex_unlock(&ee_lock);
    RET(id);
}

static void offer_token(void) {
    check_thread_termination();
    int self, next;
    if (in_intr) return;
    pthread_mutex_lock(&ee_lock);
    self = self_tid;
    if (self < 0 || current_tid != self || kernel_exit_requested) {
        pthread_mutex_unlock(&ee_lock);
        return;
    }
    next = pick_next(self);
    if (next < 0 || threads[next].cur_prio >= threads[self].cur_prio) {
        pthread_mutex_unlock(&ee_lock);
        return;
    }
    threads[self].status = THS_READY;
    current_tid = next;
    threads[next].status = THS_RUN;
    pthread_cond_signal(&threads[next].cv);
    yield_to_scheduler(self);
    threads[self].status = THS_RUN;
    current_tid = self;
    pthread_mutex_unlock(&ee_lock);
}

void ps2_preempt(void) {
    int self;
    check_thread_termination();
    if (in_intr) {
        static int said;
        if (!said && ps2_dmac_pending()) {
            said = 1;
            ps2_log("preempt: reached from inside an interrupt handler with "
                    "DMAC status %03X pending -- nothing can be delivered here",
                    ps2_dmac_pending());
        }
        return;
    }
    pthread_mutex_lock(&ee_lock);
    self = self_tid;
    if (self >= 0 && current_tid == self) {
        run_pending_vblanks(self);
        if (ps2_dmac_pending()) {
            current_tid = TID_INTR;
            in_intr = 1;
            pthread_mutex_unlock(&ee_lock);
            run_dmac_completions();
            pthread_mutex_lock(&ee_lock);
            in_intr = 0;
            current_tid = self;
        }
    }
    pthread_mutex_unlock(&ee_lock);
    offer_token();
}

static void sys_exit_thread(int delete_it) {
    int self = self_tid;
    pthread_mutex_lock(&ee_lock);
    threads[self].status = THS_DORMANT;
    threads[self].exited = 1;
    threads[self].waiting_sema = 0;
    if (delete_it) threads[self].used = 0;
    release_token(self);
    pthread_mutex_unlock(&ee_lock);
    if (self == 0) {
        for (;;) {
            struct timespec ts = { 0, 20 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    pthread_exit(NULL);
}

static void sys_rotate_ready_queue(ps2_ctx *ctx) {
    int prio = (int)A0;
    int self = self_tid;
    int next = -1, i;
    pthread_mutex_lock(&ee_lock);
    if (self < 0 || current_tid != self || kernel_exit_requested) {
        pthread_mutex_unlock(&ee_lock);
        RET(self); return;
    }
    if ((int)A0 == 0) prio = threads[self].cur_prio;
    for (i = 1; i < PS2_MAX_THREADS; i++) {
        int c = (self + i) % PS2_MAX_THREADS;
        if (!threads[c].used || threads[c].exited) continue;
        if (threads[c].status != THS_READY) continue;
        if (threads[c].cur_prio != prio) continue;
        next = c;
        break;
    }
    if (next < 0) {
        int best = pick_next(self);
        if (best >= 0 && threads[best].cur_prio < threads[self].cur_prio)
            next = best;
    }
    if (next >= 0) {
        threads[self].status = THS_READY;
        current_tid = next;
        threads[next].status = THS_RUN;
        pthread_cond_signal(&threads[next].cv);
        yield_to_scheduler(self);
        threads[self].status = THS_RUN;
        current_tid = self;
    } else {
        run_pending_vblanks(self);
        next = self;
    }
    pthread_mutex_unlock(&ee_lock);
    RET(next);
}

static void dmac_mask_rebuild(void) {
    u32 m = 0;
    int i;
    for (i = 0; i < PS2_MAX_HANDLER; i++)
        if (dmac_h[i].used && (unsigned)dmac_h[i].cause < 32u)
            m |= 1u << dmac_h[i].cause;
    dmac_handler_mask = m;
}

static int add_handler(ps2_handler *tab, int cause, u32 h, u32 arg, u32 gp) {
    for (int i = 0; i < PS2_MAX_HANDLER; i++)
        if (tab[i].used && tab[i].cause == cause && tab[i].handler == h &&
            tab[i].arg == arg && tab[i].gp == gp) return i;
    for (int i = 0; i < PS2_MAX_HANDLER; i++) {
        if (tab[i].used) continue;
        tab[i].used = 1;
        tab[i].cause = cause;
        tab[i].handler = h;
        tab[i].arg = arg;
        tab[i].gp = gp;
        return i;
    }
    return -1;
}

void ps2_kernel_run_intc(ps2_ctx *ctx, int cause) {
    for (int i = 0; i < PS2_MAX_HANDLER; i++) {
        if (!intc_h[i].used || intc_h[i].cause != cause) continue;
        if (!(intc_enabled & (1u << cause))) continue;
        ctx->r[4].ud[0] = (u64)(u32)cause;
        ctx->r[5].ud[0] = intc_h[i].arg;
        ctx->r[6].ud[0] = 0;
        ctx->r[31].ud[0] = 0;
        ctx->in_interrupt = 1;
        ps2_dispatch(ctx, intc_h[i].handler);
        ctx->in_interrupt = 0;
    }
}

void ps2_kernel_run_dmac(ps2_ctx *ctx, int cause) {
    for (int i = 0; i < PS2_MAX_HANDLER; i++) {
        if (!dmac_h[i].used || dmac_h[i].cause != cause) continue;
        if (!(dmac_enabled & (1u << cause))) continue;
        { static int n; if (n < 6) { n++;
            ps2_log("dmac: running ch%d handler %08X (field %llu)", cause,
                    dmac_h[i].handler,
                    (unsigned long long)ps2_kernel_vblank_count()); } }
        ctx->r[4].ud[0] = (u64)(u32)cause;
        ctx->r[5].ud[0] = dmac_h[i].arg;
        ctx->r[6].ud[0] = 0;
        ctx->r[31].ud[0] = 0;
        ctx->in_interrupt = 1;
        ps2_dispatch(ctx, dmac_h[i].handler);
        ctx->in_interrupt = 0;
    }
}

void ps2_syscall(ps2_ctx *ctx) {
    s32 num = (s32)ctx->r[3].sw[0];
    u32 key = num < 0 ? 0u - (u32)num : (u32)num;
    if ((u32)key < 300) syscall_counts[key]++;
    switch (key) {
    case 2:
        ps2_log("kernel: SetGsCrt(inter=%u mode=%u field=%u)", A0, A1, A2);
        RET(0);
        return;
    case 112: RET(gs_imr_shadow); return;
    case 113: { u32 o = gs_imr_shadow; gs_imr_shadow = A0; RET(o); return; }
    case 115: vsync_flag_ptr = A0; vsync_flag_val = A1; RET(0); return;

    case 32: RET(sys_create_thread(ctx)); return;
    case 33: {
        int id = (int)A0, result = -1;
        pthread_mutex_lock(&ee_lock);
        if (id > 0 && id < PS2_MAX_THREADS && threads[id].used &&
            threads[id].status == THS_DORMANT) {
            threads[id].used = 0;
            __atomic_store_n(&threads[id].terminate_requested, 1, __ATOMIC_RELAXED);
            pthread_cond_signal(&threads[id].cv);
            result = 0;
        }
        pthread_mutex_unlock(&ee_lock);
        RET(result); return;
    }
    case 34: sys_start_thread(ctx); return;
    case 35:
    case 36:
        RET(0);
        sys_exit_thread(key == 36);
        return;
    case 37: {
        int id = (int)A0, join_host = 0;
        pthread_mutex_lock(&ee_lock);
        if (id <= 0 || id >= PS2_MAX_THREADS || !threads[id].used) {
            pthread_mutex_unlock(&ee_lock); RET(-1); return;
        }
        __atomic_store_n(&threads[id].terminate_requested, 1, __ATOMIC_RELAXED);
        threads[id].status = THS_DORMANT;
        pthread_cond_signal(&threads[id].cv);
        join_host = id != self_tid && threads[id].started;
        pthread_mutex_unlock(&ee_lock);
        if (join_host) {
            pthread_join(threads[id].th, NULL);
            pthread_mutex_lock(&ee_lock);
            threads[id].started = 0;
            pthread_mutex_unlock(&ee_lock);
        }
        check_thread_termination();
        RET(0); return;
    }
    case 41: {
        int id = (int)A0 ? (int)A0 : self_tid;
        int old;
        if (id <= 0 || id >= PS2_MAX_THREADS || !threads[id].used) {
            RET(-1); return;
        }
        pthread_mutex_lock(&ee_lock);
        old = threads[id].cur_prio;
        threads[id].cur_prio = (int)A1;
        pthread_mutex_unlock(&ee_lock);
        offer_token();
        RET(old); return;
    }
    case 43: sys_rotate_ready_queue(ctx); return;
    case 45: {
        pthread_mutex_lock(&ee_lock);
        make_ready((int)A0);
        pthread_mutex_unlock(&ee_lock);
        RET(0); return;
    }
    case 47: RET(self_tid); return;
    case 48: sys_refer_thread_status(ctx); return;
    case 50: {
        int self = self_tid;
        pthread_mutex_lock(&ee_lock);
        if (threads[self].wakeup_count > 0) {
            threads[self].wakeup_count--;
            pthread_mutex_unlock(&ee_lock);
            RET(0);
            return;
        }
        block_self_locked(THS_WAIT);
        pthread_mutex_unlock(&ee_lock);
        RET(0);
        return;
    }
    case 51: case 52: {
        int id = (int)A0;
        pthread_mutex_lock(&ee_lock);
        if (id > 0 && id < PS2_MAX_THREADS && threads[id].used) {
            if (threads[id].status == THS_WAIT
                || threads[id].status == THS_WAITSUSPEND) make_ready(id);
            else threads[id].wakeup_count++;
        }
        pthread_mutex_unlock(&ee_lock);
        offer_token();
        RET(id);
        return;
    }
    case 53: case 54:
        pthread_mutex_lock(&ee_lock);
        if ((int)A0 > 0 && (int)A0 < PS2_MAX_THREADS)
            threads[A0].wakeup_count = 0;
        pthread_mutex_unlock(&ee_lock);
        RET(0); return;
    case 55: case 56: {
        int id = (int)A0;
        if (id <= 0 || id >= PS2_MAX_THREADS || !threads[id].used) {
            RET(-1); return;
        }
        pthread_mutex_lock(&ee_lock);
        if (threads[id].status == THS_WAIT)
            threads[id].status = THS_WAITSUSPEND;
        else if (threads[id].status != THS_WAITSUSPEND)
            threads[id].status = THS_SUSPEND;
        pthread_mutex_unlock(&ee_lock);
        RET(0); return;
    }
    case 57: case 58: {
        int id = (int)A0;
        if (id <= 0 || id >= PS2_MAX_THREADS || !threads[id].used) {
            RET(-1); return;
        }
        pthread_mutex_lock(&ee_lock);
        if (threads[id].status == THS_WAITSUSPEND) {
            threads[id].status = THS_WAIT;
        } else if (threads[id].status == THS_SUSPEND) {
            threads[id].status = THS_READY;
            pthread_cond_signal(&threads[id].cv);
        }
        pthread_mutex_unlock(&ee_lock);
        offer_token();
        RET(0); return;
    }

    case 64: {
        pthread_mutex_lock(&ee_lock);
        int id = sys_create_sema(ctx);
        pthread_mutex_unlock(&ee_lock);
        RET(id); return;
    }
    case 65:
        pthread_mutex_lock(&ee_lock);
        if ((int)A0 > 0 && (int)A0 < PS2_MAX_SEMA) {
            semas[A0].used = 0;
            semas[A0].generation++;
            for (int i = 1; i < PS2_MAX_THREADS; i++)
                if (threads[i].waiting_sema == (int)A0) make_ready(i);
        }
        pthread_mutex_unlock(&ee_lock);
        offer_token(); RET(0); return;
    case 66: sys_signal_sema(ctx); offer_token(); return;
    case 67: sys_signal_sema(ctx); return;
    case 68: sys_wait_sema(ctx); return;
    case 69: {
        int id = (int)A0;
        pthread_mutex_lock(&ee_lock);
        if (id > 0 && id < PS2_MAX_SEMA && semas[id].used && semas[id].count > 0) {
            semas[id].count--;
            RET(id);
        } else RET(-1);
        pthread_mutex_unlock(&ee_lock);
        return;
    }
    case 70: {
        int id = (int)A0;
        pthread_mutex_lock(&ee_lock);
        if (id > 0 && id < PS2_MAX_SEMA && semas[id].used && semas[id].count > 0)
            { semas[id].count--; RET(id); }
        else RET(-1);
        pthread_mutex_unlock(&ee_lock);
        return;
    }
    case 71: case 72: {
        int id = (int)A0;
        pthread_mutex_lock(&ee_lock);
        u32 p = A1;
        if (id > 0 && id < PS2_MAX_SEMA && semas[id].used && p) {
            s32w(p + 0, (u32)semas[id].count);
            s32w(p + 4, (u32)semas[id].max_count);
            s32w(p + 8, (u32)semas[id].init_count);
            s32w(p + 12, (u32)semas[id].wait_threads);
            s32w(p + 16, semas[id].attr);
            s32w(p + 20, semas[id].option);
            RET(id);
        } else RET(-1);
        pthread_mutex_unlock(&ee_lock);
        return;
    }

    case 16: RET(add_handler(intc_h, (int)A0, A1, A3, (u32)ctx->r[28].ud[0]));
             return;
    case 17: {
        int i = (int)A1;
        if (i < 0 || i >= PS2_MAX_HANDLER || !intc_h[i].used || intc_h[i].cause != (int)A0) { RET(-1); return; }
        intc_h[i].used = 0;
        RET(0); return;
    }
    case 18:
        ps2_log("dmac: AddDmacHandler(ch=%d, handler=%08X, next=%d) at field %llu",
                (int)A0, (u32)A1, (int)A2,
                (unsigned long long)ps2_kernel_vblank_count());
        { int h = add_handler(dmac_h, (int)A0, A1, A3, (u32)ctx->r[28].ud[0]);
          dmac_mask_rebuild(); RET(h); }
             return;
    case 19: {
        int i = (int)A1;
        if (i < 0 || i >= PS2_MAX_HANDLER || !dmac_h[i].used || dmac_h[i].cause != (int)A0) { RET(-1); return; }
        dmac_h[i].used = 0;
        dmac_mask_rebuild();
        RET(0); return;
    }
    case 20: case 26: case 92: intc_enabled |= 1u << (A0 & 31); RET(1); return;
    case 21: case 27: case 93: intc_enabled &= ~(1u << (A0 & 31)); RET(1); return;
    case 22: case 28: case 94:
        ps2_log("dmac: EnableDmac(ch=%d) at field %llu", (int)A0,
                (unsigned long long)ps2_kernel_vblank_count());
        dmac_enabled |= 1u << (A0 & 31); RET(1); return;
    case 23: case 29: case 95: dmac_enabled &= ~(1u << (A0 & 31)); RET(1); return;

    case 100: case 101: RET(0); return;
    case 62: RET(heap_end); return;
    case 127: RET(PS2_RAM_SIZE); return;
    case 90: {
        u32 dst = A0, src = A1, len = A2;
        for (u32 i = 0; i < len; i++) ps2_w8(dst + i, ps2_r8(src + i));
        RET(0); return;
    }
    case 91: RET(ps2_entry_point); return;

    case 74: osd_param = A0; RET(0); return;
    case 75: s32w(A0, osd_param); RET(0); return;
    case 110: osd_param2 = A0; RET(0); return;
    case 111: s32w(A0, osd_param2); RET(0); return;
    case 125: RET(0); return;
    case 126: RET(0); return;

    case 13: case 14: case 15: case 84: case 85: case 86: case 87:
    case 88: case 130:
        RET(0); return;
    case 131: {
        u32 start = A0, end = A1, want = A2, a;
        for (a = start; a < end; a += 4) {
            if (ps2_r32(a) == want) { RET(a); return; }
        }
        RET(start < end ? end : start);
        return;
    }
    case 89: RET(0); return;
    case 96: RET(A0 & 0x1FFFFFFFu); return;
    case 97: case 98: RET(0); return;
    case 99: RET(ps2_cop0_read(ctx, (int)A0)); return;
    case 102: case 106: RET(0); return;
    case 116: RET(0); return;
    case 117:
        RET(0); return;
    case 124: RET(-1); return;

    case 107: RET(0); return;
    case -118:
    case 118: RET(-1); return;
    case -119:
    case 119: {
        u32 sdd = A0;
        int n = (int)A1, i;
        for (i = 0; i < n; i++) {
            u32 e = sdd + (u32)i * 16u;
            u32 src = ps2_r32(e + 0), dst = ps2_r32(e + 4), size = ps2_r32(e + 8);
            if (!size) continue;
            for (u32 k = 0; k < size; k++) ps2_w8(dst + k, ps2_r8(src + k));
            sif_dma_bytes += size;
        }
        sif_dma_id++;
        if (!sif_dma_id) sif_dma_id = 1;
        RET(sif_dma_id);
        return;
    }
    case -120:
    case 120: RET(1); return;
    case 121: ps2_sif_set_reg(A0, A1); RET(0); return;
    case 122: RET(ps2_sif_get_reg(A0)); return;

    case 4:
        ps2_log("kernel: guest called _Exit(%d)", (int)A0);
        kernel_exit_requested = 1;
        ps2_dump_trace("guest exit");
        exit((int)A0);
        return;
    case 6: case 7:
        ps2_log("kernel: guest requested exec of %08X -- not supported", A0);
        RET(-1);
        return;
    case 60: {
        u32 stack = A1, size = A2;
        if (size == 0 || size > PS2_RAM_SIZE) size = 0x100000u;
        main_stack_base = (stack == 0xFFFFFFFFu) ? (PS2_RAM_SIZE - size) : stack;
        main_stack_size = size;
        ps2_log("kernel: InitMainThread(gp=%08X stack=%08X size=%08X) "
                "-> sp=%08X", A0, stack, size,
                main_stack_base + size - 0x40u);
        RET(main_stack_base + size - 0x40u);
        return;
    }
    case 61: {
        u32 heap = A0, size = A1;
        heap_base = heap;
        heap_end = (size == 0xFFFFFFFFu) ? main_stack_base : heap + size;
        ps2_log("kernel: InitHeap(base=%08X size=%08X) -> end=%08X",
                heap, size, heap_end);
        RET(heap_end);
        return;
    }
    case 63: case 73: case 80: case 81: case 82: case 83:
        RET(0); return;
    default:
        if ((u32)key < 300) unhandled_syscalls[key]++;
        else unhandled_out_of_range++;
        RET(0);
        return;
    }
}

static void deliver_vblank(void) {
    memset(&intr_ctx.r[0], 0, sizeof(intr_ctx.r));
    intr_ctx.r[29].ud[0] = intr_stack_top;
    intr_ctx.cop0[12] = 0x70000000u;
    ps2_gs_vblank();
    ps2_timers_tick();
    ps2_pad_latch();
    if (vsync_flag_ptr) ps2_w32(vsync_flag_ptr, vsync_flag_val);
    ps2_kernel_run_intc(&intr_ctx, 2);
    ps2_kernel_run_intc(&intr_ctx, 3);
    {
        u32 pend = ps2_intc_pending();
        for (int c = 0; c < 15 && pend; c++) {
            if (!(pend & (1u << c))) continue;
            pend &= ~(1u << c);
            ps2_kernel_run_intc(&intr_ctx, c);
            ps2_intc_ack(c);
        }
    }
    run_dmac_completions();
    vblank_delivered++;
    {
        static int checked, want;
        if (!checked) {
            const char *e = getenv("PS2_DIAG_AFTER");
            checked = 1;
            if (e) { want = (int)strtoul(e, NULL, 0); ps2_diag_armed = 0; }
        }
        if (want && !ps2_diag_armed && vblank_delivered >= (u64)want) {
            ps2_diag_armed = 1;
            ps2_log("diag: censuses and texture dumps armed at vblank %llu",
                    (unsigned long long)vblank_delivered);
        }
    }
    {
        static int capture_phase;
        static int capture_was_armed;
        static u64 capture_prims;
        static int capture_wait;
        static u64 capture_at = 0;
        static int capture_at_read;
        if (!capture_at_read) {
            const char *e = getenv("PS2_CAPTURE_AT");
            capture_at_read = 1;
            capture_at = e ? strtoull(e, NULL, 0) : 0;
        }
        if (capture_phase == 1) {
            u64 prims, pixels, regs;
            ps2_gs_stats(&prims, &pixels, &regs);
            if (prims == capture_prims && ++capture_wait < 16) {
            } else {
            capture_phase = 0;
            ps2_capture_report("F9");
            ps2_diag_armed = capture_was_armed;
            }
        } else if (capture_at && vblank_delivered == capture_at) {
            capture_phase = 1;
            capture_was_armed = ps2_diag_armed;
            ps2_diag_armed = 1;
            { u64 p, px, r; ps2_gs_stats(&p, &px, &r);
              capture_prims = p; capture_wait = 0; }
            ps2_gif_dump_arm(6);
            ps2_gs_census_reset();
            ps2_log("capture: field %llu armed (PS2_CAPTURE_AT)",
                    (unsigned long long)vblank_delivered);
        } else if (ps2_capture_request) {
            ps2_capture_request = 0;
            capture_phase = 1;
            capture_was_armed = ps2_diag_armed;
            ps2_diag_armed = 1;
            { u64 p, px, r; ps2_gs_stats(&p, &px, &r);
              capture_prims = p; capture_wait = 0; }
            ps2_gif_dump_arm(6);
            ps2_gs_census_reset();
            ps2_log("capture: field %llu armed",
                    (unsigned long long)vblank_delivered);
        }
    }
    if ((vblank_delivered % 600ull) == 0ull) {
        static double last_wall;
        static u64 last_field, last_missed;
        double now = ps2_wall_seconds();
        double dt = now - last_wall;
        u64 missed = __atomic_load_n(&vblank_missed, __ATOMIC_RELAXED);
        if (last_wall > 0.0 && dt > 0.0) {
            u64 got = vblank_delivered - last_field;
            u64 lost = missed - last_missed;
            ps2_log("alive: vblank pump at field %llu -- %.1f fields/s over "
                    "the last %llu (%.2f ms/field); %llu dropped, "
                    "guest clock at %.0f%% of real time",
                    (unsigned long long)vblank_delivered,
                    (double)got / dt,
                    (unsigned long long)got,
                    dt * 1000.0 / (double)got,
                    (unsigned long long)lost,
                    100.0 * (double)got / (double)(got + lost));
        } else {
            ps2_log("alive: vblank pump at field %llu",
                    (unsigned long long)vblank_delivered);
        }
        last_wall = now;
        last_field = vblank_delivered;
        last_missed = missed;
    }
    if (ps2_state_dump_pending()) {
        static unsigned waited;
        if (pthread_mutex_trylock(&ee_lock) == 0) {
            waited = 0;
            ps2_state_dump_numbered("EE idle, consistent");
            pthread_mutex_unlock(&ee_lock);
        } else if (++waited >= 60u) {
            waited = 0;
            ps2_state_dump_numbered("EE LOCK HELD -- copy may be torn");
        }
    }
    if (!ps2_gs_present()) ps2_request_exit();
    { extern void ps2_speed_sample(u64, u64);
      ps2_speed_sample(vblank_delivered, __atomic_load_n(&vblank_missed, __ATOMIC_RELAXED)); }
    {
        unsigned budget = ps2_vblank_budget();
        if (ps2_exit_pending()) ps2_finish("window closed");
        if (budget && vblank_delivered >= budget) ps2_finish("frame budget");
    }
}

static void report_stall(u64 fields, unsigned secs) {
    ps2_log("");
    ps2_log("==== STALL: no vertical blank delivered for %u s "
            "(%llu fields so far) ====", secs, (unsigned long long)fields);
    if (secs >= 20u) {
        ps2_log("==== giving up after %u s: the guest is not advancing ====",
                secs);
        ps2_ipu_report();
        ps2_dump_trace("stall");
        ps2_sampler_report(24);
        ps2_prof_report(30);
        ps2_state_dump_if_armed();
        fflush(stderr);
        _exit(3);
    }
    {   int i, nh = 0;
        for (i = 0; i < PS2_MAX_HANDLER; i++) if (dmac_h[i].used) nh++;
        ps2_log("DMAC: status=%03X enabled=%03X, %d handler(s) registered",
                ps2_dmac_pending(), dmac_enabled, nh);
        for (i = 0; i < PS2_MAX_HANDLER; i++)
            if (dmac_h[i].used)
                ps2_log("   handler ch=%d -> %08X", dmac_h[i].cause,
                        dmac_h[i].handler);
    }
    ps2_log("EE token holder: %s",
            current_tid == TID_NONE ? "nobody"
          : current_tid == TID_INTR ? "an interrupt handler"
          : "thread (see below)");
    for (int i = 0; i < PS2_MAX_THREADS; i++) {
        if (!threads[i].used) continue;
        ps2_log("  thread %-3d status=%02X prio=%-3d entry=%08X "
                "waiting_sema=%-3d wakeups=%d%s",
                i, threads[i].status, threads[i].cur_prio, threads[i].entry,
                threads[i].waiting_sema, threads[i].wakeup_count,
                i == current_tid ? "   <-- holds the EE" : "");
    }
    for (int i = 0; i < PS2_MAX_SEMA; i++)
        if (semas[i].used && semas[i].wait_threads)
            ps2_log("  sema   %-3d count=%-4d max=%-4d waiters=%d",
                    i, semas[i].count, semas[i].max_count,
                    semas[i].wait_threads);
    ps2_dump_trace("stall");
    ps2_log("==== end of stall report ====");
    ps2_log("");
}

#define FIELD_NS 16683333ull

static u64 mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static void sleep_until(u64 deadline) {
    for (;;) {
        u64 now = mono_ns();
        if (now >= deadline) return;
        u64 d = deadline - now;
#ifdef _WIN32
        if (field_timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((d + 99u) / 100u);
            if (SetWaitableTimer(field_timer, &due, 0, NULL, NULL, FALSE)
                && WaitForSingleObject(field_timer, INFINITE) == WAIT_OBJECT_0)
                continue;
            ps2_log("clock: high-resolution wait failed; using fallback sleep");
            CloseHandle(field_timer);
            field_timer = NULL;
            continue;
        }
#endif
        struct timespec ts;
        ts.tv_sec = (time_t)(d / 1000000000ull);
        ts.tv_nsec = (long)(d % 1000000000ull);
        nanosleep(&ts, NULL);
    }
}

static u64 field_ns(void) {
    static u64 v;
    if (!v) {
        const char *e = getenv("PS2_FIELD_HZ");
        double hz = e && *e ? atof(e) : 0.0;
        v = FIELD_NS;
        if (hz > 1.0 && hz < 10000.0) {
            v = (u64)(1e9 / hz);
            ps2_log("field clock: %.2f Hz (%.2f ms) -- benchmark rate, not "
                    "real time", hz, (double)v / 1e6);
        }
    }
    return v;
}

static void *vblank_timer(void *unused) {
    u64 last_seen = 0;
    u64 fns = field_ns();
    u64 deadline = mono_ns() + fns;
    unsigned quiet = 0, next_report = 5;
    (void)unused;
#ifdef _WIN32
    field_timer = CreateWaitableTimerExW(NULL, NULL,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
    ps2_log("clock: %s; absolute 59.94-Hz field deadlines (unless PS2_FIELD_HZ overrides)",
        field_timer ? "Windows high-resolution waitable timer" : "fallback nanosleep");
#endif
    while (vblank_running) {
        u64 now;
        sleep_until(deadline);
        deadline += fns;
        now = mono_ns();
        if (now > deadline + 4ull * fns) deadline = now + fns;
        if (__atomic_load_n(&vblank_pending, __ATOMIC_RELAXED) < 4)
            __atomic_fetch_add(&vblank_pending, 1, __ATOMIC_RELAXED);
        else
            __atomic_fetch_add(&vblank_missed, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&vblank_generated, 1, __ATOMIC_RELAXED);
        now = __atomic_load_n(&vblank_delivered, __ATOMIC_RELAXED);
        if (now != last_seen) {
            last_seen = now;
            quiet = 0;
            next_report = 5;
            continue;
        }
        if (++quiet < next_report * 60u) continue;
        if (pthread_mutex_trylock(&ee_lock) == 0) {
            report_stall(now, quiet / 60u);
            pthread_mutex_unlock(&ee_lock);
        } else {
            ps2_log("==== STALL: no vertical blank for %u s, and the kernel "
                    "lock is held ====", quiet / 60u);
        }
        if (quiet / 60u >= 15u) {
            ps2_log("==== giving up after %u s: the guest is not advancing ====",
                    quiet / 60u);
            ps2_ipu_report();
            ps2_dump_trace("stall");
            ps2_state_dump_if_armed();
            fflush(stderr);
            _exit(3);
        }
        next_report = quiet / 60u + 30u;
    }
#ifdef _WIN32
    if (field_timer) { CloseHandle(field_timer); field_timer = NULL; }
#endif
    return NULL;
}

void ps2_kernel_state_save(ps2_state_put put, void *ud) {
    put(ud, "threads", threads, sizeof threads);
    put(ud, "semas", semas, sizeof semas);
    put(ud, "intc_h", intc_h, sizeof intc_h);
    put(ud, "dmac_h", dmac_h, sizeof dmac_h);
}

void ps2_kernel_vblank(ps2_ctx *ctx) {
    (void)ctx;
    __atomic_fetch_add(&vblank_pending, 1, __ATOMIC_RELAXED);
}

void ps2_kernel_poll_vblank(void) {
    int self = self_tid;
    if (in_intr) return;
    if (!__atomic_load_n(&vblank_pending, __ATOMIC_RELAXED)) return;
    if (self < 0) return;
    pthread_mutex_lock(&ee_lock);
    run_pending_vblanks(self);
    pthread_mutex_unlock(&ee_lock);
    offer_token();
}

u64 ps2_kernel_vblank_count(void) { return vblank_delivered; }

static const char *ths_name(int st) {
    switch (st) {
    case THS_RUN:      return "RUN";
    case THS_READY:    return "READY";
    case THS_WAIT:     return "WAIT";
    case THS_SUSPEND:  return "SUSPEND";
    case THS_DORMANT:  return "DORMANT";
    default:           return "?";
    }
}

static void ee_statecap(const char *dir) {
    char p[400];
    FILE *f;
    int i;

    snprintf(p, sizeof p, "%s/ee.txt", dir);
    f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "# EE kernel state.  `sp` and `ra` are guest addresses: the\n"
               "# call chain is in ram.bin, walkable with read_ram.py.\n"
               "# Recompiled code runs on native stacks, so there is no host\n"
               "# backtrace to record here -- see ps2_state.c's note.\n\n");
    fprintf(f, "current thread %d\n\n", current_tid);

    fprintf(f, "== threads ==\n");
    for (i = 0; i < PS2_MAX_THREADS; i++) {
        ps2_thread *t = &threads[i];
        if (!t->used) continue;
        fprintf(f, "  tid %-3d %-8s prio %3d/%-3d entry %08X stack %08X+%X\n",
                i, ths_name(t->status), t->cur_prio, t->init_prio,
                t->entry, t->stack, t->stack_size);
        fprintf(f, "          wakeups %d  waiting_sema %d  started %d exited %d\n",
                t->wakeup_count, t->waiting_sema, t->started, t->exited);
        fprintf(f, "          sp=%016llX ra=%016llX gp=%08X pc=%08X\n",
                (unsigned long long)t->ctx.r[29].ud[0],
                (unsigned long long)t->ctx.r[31].ud[0],
                t->gp, t->ctx.pc);
    }

    fprintf(f, "\n== semaphores ==\n");
    for (i = 0; i < PS2_MAX_SEMA; i++) {
        if (!semas[i].used) continue;
        fprintf(f, "  sem %-3d count %d/%d (init %d)  %d thread%s waiting\n",
                i, semas[i].count, semas[i].max_count, semas[i].init_count,
                semas[i].wait_threads, semas[i].wait_threads == 1 ? "" : "s");
    }

    fprintf(f, "\n== full register files ==\n");
    for (i = 0; i < PS2_MAX_THREADS; i++) {
        ps2_thread *t = &threads[i];
        int r;
        if (!t->used) continue;
        fprintf(f, "  -- tid %d --\n", i);
        for (r = 0; r < 32; r++)
            fprintf(f, "    r%-2d = %016llX_%016llX%s", r,
                    (unsigned long long)t->ctx.r[r].ud[1],
                    (unsigned long long)t->ctx.r[r].ud[0],
                    (r & 1) ? "\n" : "  ");
        fprintf(f, "\n");
    }
    fclose(f);
}

void ps2_kernel_init(void) {
    ps2_statecap_register("ee", ee_statecap);
    self_tid = 0;
    threads[0].used = 1;
    threads[0].status = THS_RUN;
    threads[0].cur_prio = 64;
    threads[0].init_prio = 64;
    pthread_cond_init(&threads[0].cv, NULL);
    current_tid = 0;
    intr_stack_top = 0x01EFFF00u;
    vblank_running = 1;
    pthread_create(&vblank_th, NULL, vblank_timer, NULL);
    pthread_detach(vblank_th);
}

void ps2_timers_report(void);

void ps2_kernel_report(void) {
    ps2_timers_report();
    ps2_log("---- kernel state ----");
    ps2_log("vblanks delivered   : %llu", (unsigned long long)vblank_delivered);
    for (int i = 0; i < PS2_MAX_THREADS; i++) {
        if (!threads[i].used) continue;
        ps2_log("thread %-3d status=%02X prio=%-3d entry=%08X waiting_sema=%d%s",
                i, threads[i].status, threads[i].cur_prio, threads[i].entry,
                threads[i].waiting_sema, i == current_tid ? "  <-- current" : "");
    }
    for (int i = 0; i < PS2_MAX_SEMA; i++)
        if (semas[i].used)
            ps2_log("sema   %-3d count=%-4d max=%-4d waiters=%d",
                    i, semas[i].count, semas[i].max_count, semas[i].wait_threads);
    ps2_log("---- unhandled syscall usage ----");
    for (int i = 0; i < 300; i++)
        if (unhandled_syscalls[i]) ps2_log("   unimplemented syscall %d x%llu", i,
            (unsigned long long)unhandled_syscalls[i]);
    if (unhandled_out_of_range) ps2_log("   invalid syscall numbers x%llu",
        (unsigned long long)unhandled_out_of_range);
    ps2_log("---- syscall usage ----");
    for (int i = 0; i < 300; i++)
        if (syscall_counts[i])
            ps2_log("   syscall %3d  x%llu", i,
                    (unsigned long long)syscall_counts[i]);
}
