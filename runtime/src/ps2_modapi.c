#include "ps2_runtime.h"
#include "ps2_hle.h"
#include "ps2_hook.h"
#include "ps2_modapi.h"
#include "ps2_params.h"
#include "ps2_patch.h"
#include "ps2_vfs.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ps2_mkdir_p(const char *path);

typedef struct { char *key, *val; } store_kv;

typedef struct {
    ac5_api api;
    char id[64];
    const char *(*config_get)(const char *mod_id, const char *key);
    u64 refused;
    store_kv *store;
    unsigned nstore, cstore;
    int store_loaded, store_dirty;
} mod_api;

static mod_api **apis;
static unsigned napis, capis;
static __thread mod_api *current;

static mod_api *as_mod(const ac5_api *api) { return (mod_api *)(void *)api; }

const ac5_api *ps2_modapi_enter(const ac5_api *api) {
    mod_api *prev = current;
    current = api ? as_mod(api) : NULL;
    return prev ? &prev->api : NULL;
}

void ps2_modapi_leave(const ac5_api *previous) {
    current = previous ? as_mod(previous) : NULL;
}

static const char *cur_id(void) { return current ? current->id : "?"; }
static int cur_priority(void) { return current ? current->api.priority : 0; }

static void api_log(const char *fmt, ...) {
    char line[960];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    ps2_log("mod[%s]: %s", cur_id(), line);
}

static int on_ee(const char *what) {
    if (ps2_kernel_on_ee_thread()) return 1;
    if (current && !current->refused++)
        ps2_log("mod[%s]: %s from a thread that does not hold the EE -- "
                "refused, here and from now on (counted in the run summary)",
                current->id, what);
    return 0;
}

static u8 *page_of(u32 a) { return ps2_pt[a >> PS2_PAGE_BITS]; }

static void mem_get(u8 *dst, u32 a, u32 n) {
    while (n) {
        u32 off = a & PS2_PAGE_MASK, chunk = PS2_PAGE_SIZE - off;
        u8 *p = page_of(a);
        if (chunk > n) chunk = n;
        if (p) memcpy(dst, p + off, chunk);
        else memset(dst, 0, chunk);
        dst += chunk; a += chunk; n -= chunk;
    }
}

static void mem_put(u32 a, const u8 *src, u32 n) {
    while (n) {
        u32 off = a & PS2_PAGE_MASK, chunk = PS2_PAGE_SIZE - off;
        u8 *p = page_of(a);
        if (chunk > n) chunk = n;
        if (p) memcpy(p + off, src, chunk);
        src += chunk; a += chunk; n -= chunk;
    }
}

static uint8_t api_read8(uint32_t a) {
    u8 v = 0;
    if (on_ee("read8")) mem_get(&v, a, 1);
    return v;
}
static uint16_t api_read16(uint32_t a) {
    u16 v = 0;
    if (on_ee("read16")) mem_get((u8 *)&v, a, 2);
    return v;
}
static uint32_t api_read32(uint32_t a) {
    u32 v = 0;
    if (on_ee("read32")) mem_get((u8 *)&v, a, 4);
    return v;
}
static float api_read_float(uint32_t a) {
    u32 raw = api_read32(a);
    float f;
    memcpy(&f, &raw, 4);
    return f;
}
static void api_write8(uint32_t a, uint8_t v) {
    if (on_ee("write8")) mem_put(a, &v, 1);
}
static void api_write16(uint32_t a, uint16_t v) {
    if (on_ee("write16")) mem_put(a, (const u8 *)&v, 2);
}
static void api_write32(uint32_t a, uint32_t v) {
    if (on_ee("write32")) mem_put(a, (const u8 *)&v, 4);
}
static void api_write_float(uint32_t a, float v) {
    u32 raw;
    memcpy(&raw, &v, 4);
    api_write32(a, raw);
}
static void api_read_bytes(void *dst, uint32_t a, uint32_t n) {
    if (on_ee("read_bytes")) mem_get((u8 *)dst, a, n);
    else memset(dst, 0, n);
}
static void api_write_bytes(uint32_t a, const void *src, uint32_t n) {
    if (on_ee("write_bytes")) mem_put(a, (const u8 *)src, n);
}

static uint32_t api_arg(ac5_ctx *ctx, int n) {
    if (!ctx || n < 0 || !on_ee("arg")) return 0;
    if (n < 8) return ps2_arg((ps2_ctx *)ctx, n);
    return api_read32((u32)((ps2_ctx *)ctx)->r[29].ud[0] + 4u * (u32)(n - 8));
}
static void api_set_return(ac5_ctx *ctx, uint32_t v) {
    if (ctx && on_ee("set_return")) ((ps2_ctx *)ctx)->r[2].sd[0] = (s64)(s32)v;
}
static uint32_t api_reg(ac5_ctx *ctx, int n) {
    if (!ctx || n < 0 || n > 31 || !on_ee("reg")) return 0;
    return (uint32_t)((ps2_ctx *)ctx)->r[n].uw[0];
}
static void api_set_reg(ac5_ctx *ctx, int n, uint32_t v) {
    if (!ctx || n <= 0 || n > 31) return;
    if (on_ee("set_reg")) ((ps2_ctx *)ctx)->r[n].sd[0] = (s64)(s32)v;
}
static uint32_t api_entry_arg(ac5_ctx *ctx, int n) {
    int ok;
    u32 v;
    if (!on_ee("entry_arg")) return 0;
    v = ps2_hook_entry_arg(n, &ok);
    if (!ok) return api_arg(ctx, n);
    return v;
}
static uint32_t api_return_value(ac5_ctx *ctx) { return api_reg(ctx, 2); }
static float api_reg_float(ac5_ctx *ctx, int n) {
    if (!ctx || n < 0 || n > 31 || !on_ee("reg_float")) return 0.0f;
    return ((ps2_ctx *)ctx)->f[n].f;
}
static void api_set_reg_float(ac5_ctx *ctx, int n, float v) {
    if (ctx && n >= 0 && n <= 31 && on_ee("set_reg_float"))
        ((ps2_ctx *)ctx)->f[n].f = v;
}
static float api_return_float(ac5_ctx *ctx) { return api_reg_float(ctx, 0); }
static void api_set_return_float(ac5_ctx *ctx, float v) {
    api_set_reg_float(ctx, 0, v);
}

typedef struct {
    ac5_hook_fn fn;
    void *user;
    mod_api *owner;
} native_hook;

static int native_hook_call(ps2_ctx *ctx, void *u) {
    native_hook *h = (native_hook *)u;
    mod_api *prev = current;
    int handled;
    current = h->owner;
    handled = h->fn((ac5_ctx *)ctx, h->user);
    current = prev;
    return handled;
}

static native_hook *wrap(ac5_hook_fn fn, void *user) {
    native_hook *h = (native_hook *)malloc(sizeof *h);
    if (!h) ps2_fatal("mod: out of memory attaching a hook");
    h->fn = fn;
    h->user = user;
    h->owner = current;
    return h;
}

static int api_hook_before(uint32_t addr, ac5_hook_fn fn, void *user, int prio) {
    native_hook *h;
    int handle;
    if (!fn || !on_ee("hook_before")) return -1;
    h = wrap(fn, user);
    handle = ps2_hook_before(addr, native_hook_call, h, prio, cur_id());
    if (handle < 0) free(h);
    return handle;
}
static int api_hook_after(uint32_t addr, ac5_hook_fn fn, void *user, int prio) {
    native_hook *h;
    int handle;
    if (!fn || !on_ee("hook_after")) return -1;
    h = wrap(fn, user);
    handle = ps2_hook_after(addr, native_hook_call, h, prio, cur_id());
    if (handle < 0) free(h);
    return handle;
}
static int api_hook_replace(uint32_t addr, ac5_hook_fn fn, void *user) {
    native_hook *h;
    int handle;
    if (!fn || !on_ee("hook_replace")) return -1;
    h = wrap(fn, user);
    handle = ps2_hook_replace(addr, native_hook_call, h, cur_priority(), cur_id());
    if (handle < 0) free(h);
    return handle;
}
static int api_unhook(int handle) {
    return on_ee("unhook") ? ps2_hook_remove(handle) : -1;
}
static void api_call_original(uint32_t addr, ac5_ctx *ctx) {
    if (ctx && on_ee("call_original"))
        ps2_hook_call_original(addr, (ps2_ctx *)ctx);
}
static int api_redirect(uint32_t from, uint32_t to) {
    if (!on_ee("redirect")) return -1;
    return ps2_hook_redirect(from, to, cur_priority(), cur_id());
}

static uint32_t api_call_guest(ac5_ctx *ctx, uint32_t addr,
                               const uint32_t *args, int nargs) {
    ps2_ctx sub;
    if (!ctx || !on_ee("call_guest")) return 0;
    if (nargs < 0 || nargs > 8 || (nargs && !args)) {
        ps2_log("mod[%s]: call_guest(%08X) with %d arguments refused -- up to "
                "eight fit in registers, and a ninth would overwrite the "
                "caller's own stack arguments", cur_id(), addr, nargs);
        return 0;
    }
    sub = *(ps2_ctx *)ctx;
    for (int i = 0; i < nargs; i++)
        sub.r[i < 4 ? 4 + i : 8 + (i - 4)].sd[0] = (s64)(s32)args[i];
    sub.r[31].ud[0] = 0;
    ps2_dispatch(&sub, addr);
    return (uint32_t)sub.r[2].uw[0];
}

enum { CB_FREE, CB_FIELD, CB_FRAME, CB_SCENE, CB_TIMER, CB_INPUT, CB_SHUTDOWN };

typedef struct {
    int kind, handle;
    union {
        ac5_field_fn field;
        ac5_frame_fn frame;
        ac5_scene_fn scene;
        ac5_input_fn input;
    } fn;
    void *user;
    mod_api *owner;
    u32 period;
    u64 due;
    int repeat;
} mod_cb;

static mod_cb *cbs;
static unsigned ncbs, ccbs;
static int next_cb_handle = 1;
static u64 fields_elapsed, frames_seen;
static unsigned counts[CB_SHUTDOWN + 1];

static int cb_add(int kind, void *fn, void *user, u32 period, int repeat) {
    unsigned i;
    for (i = 0; i < ncbs && cbs[i].kind != CB_FREE; i++) {}
    if (i == ncbs) {
        if (ncbs == ccbs) {
            mod_cb *grown;
            ccbs = ccbs ? ccbs * 2u : 32u;
            grown = (mod_cb *)realloc(cbs, ccbs * sizeof *cbs);
            if (!grown) ps2_fatal("mod: out of memory registering a callback");
            cbs = grown;
        }
        ncbs++;
    }
    memset(&cbs[i], 0, sizeof cbs[i]);
    cbs[i].kind = kind;
    cbs[i].handle = next_cb_handle++;
    cbs[i].fn.field = (ac5_field_fn)fn;
    cbs[i].user = user;
    cbs[i].owner = current;
    cbs[i].period = period;
    cbs[i].due = fields_elapsed + period;
    cbs[i].repeat = repeat;
    counts[kind]++;
    return cbs[i].handle;
}

static int api_cancel(int handle) {
    if (!on_ee("cancel")) return -1;
    for (unsigned i = 0; i < ncbs; i++) {
        if (cbs[i].kind == CB_FREE || cbs[i].handle != handle) continue;
        counts[cbs[i].kind]--;
        cbs[i].kind = CB_FREE;
        return 0;
    }
    return -1;
}

static int api_on_field(ac5_field_fn fn, void *user) {
    if (!fn || !on_ee("on_field")) return -1;
    return cb_add(CB_FIELD, (void *)fn, user, 0, 0);
}

static int api_timer(uint32_t fields, int repeat, ac5_field_fn fn, void *user) {
    if (!fn || !on_ee("timer")) return -1;
    if (!fields) fields = 1;
    return cb_add(CB_TIMER, (void *)fn, user, fields, repeat);
}

static int api_on_shutdown(ac5_field_fn fn, void *user) {
    if (!fn || !on_ee("on_shutdown")) return -1;
    return cb_add(CB_SHUTDOWN, (void *)fn, user, 0, 0);
}

#define AC5_SCENE_DISPATCH 0x0031CF00u
#define AC5_MAIN_MACHINE   0x004459ACu

static struct { u32 obj; int key; } machines[8];
static unsigned nmachines;
static int events_hooked;

static int scene_dispatch_hook(ps2_ctx *ctx, void *user) {
    u32 obj = ps2_arg(ctx, 0);
    int major = ps2_r8(obj + 8u), minor = ps2_r8(obj + 9u);
    int key = (major << 8) | minor;
    unsigned m;
    (void)user;
    for (m = 0; m < nmachines && machines[m].obj != obj; m++) {}
    if (m == nmachines && nmachines < 8) {
        machines[m].obj = obj;
        machines[m].key = -1;
        nmachines++;
    }
    if (m < nmachines && machines[m].key != key) {
        int prev = machines[m].key;
        machines[m].key = key;
        for (unsigned i = 0; i < ncbs; i++) {
            mod_cb cb = cbs[i];
            mod_api *save;
            if (cb.kind != CB_SCENE) continue;
            save = current;
            current = cb.owner;
            cb.fn.scene((ac5_ctx *)ctx, (int)m, major, minor,
                        prev < 0 ? -1 : prev >> 8, prev < 0 ? -1 : prev & 255,
                        cb.user);
            current = save;
        }
    }
    if (obj == ps2_r32(AC5_MAIN_MACHINE)) {
        frames_seen++;
        for (unsigned i = 0; i < ncbs; i++) {
            mod_cb cb = cbs[i];
            mod_api *save;
            if (cb.kind != CB_FRAME) continue;
            save = current;
            current = cb.owner;
            cb.fn.frame((ac5_ctx *)ctx, cb.user);
            current = save;
        }
    }
    return 0;
}

static int events_hook(void) {
    if (events_hooked) return 0;
    if (ps2_hook_before(AC5_SCENE_DISPATCH, scene_dispatch_hook, NULL, INT_MAX,
                        "mod host (frame and scene events)") < 0)
        return -1;
    events_hooked = 1;
    return 0;
}

static int api_on_frame(ac5_frame_fn fn, void *user) {
    if (!fn || !on_ee("on_frame") || events_hook() != 0) return -1;
    return cb_add(CB_FRAME, (void *)fn, user, 0, 0);
}

static int api_on_scene(ac5_scene_fn fn, void *user) {
    if (!fn || !on_ee("on_scene") || events_hook() != 0) return -1;
    return cb_add(CB_SCENE, (void *)fn, user, 0, 0);
}

static void pad_from_host(const ps2_pad_state *h, ac5_pad *p) {
    p->connected = (uint8_t)(h->connected != 0);
    p->buttons = h->buttons;
    p->lx = h->lx; p->ly = h->ly; p->rx = h->rx; p->ry = h->ry;
    p->l2 = h->l2; p->r2 = h->r2;
}

static int api_pad(int port, ac5_pad *out) {
    if (port < 0 || port >= PS2_PAD_PORTS || !out || !on_ee("pad")) return -1;
    pad_from_host(&ps2_pad_host[port], out);
    return 0;
}

static int api_on_input(ac5_input_fn fn, void *user) {
    if (!fn || !on_ee("on_input")) return -1;
    return cb_add(CB_INPUT, (void *)fn, user, 0, 0);
}

void ps2_modapi_input(ps2_pad_state *host, int ports) {
    ac5_pad pads[AC5_PAD_PORTS];
    mod_api *save = current;
    if (!counts[CB_INPUT]) return;
    if (ports > AC5_PAD_PORTS) ports = AC5_PAD_PORTS;
    for (int i = 0; i < ports; i++) pad_from_host(&host[i], &pads[i]);
    for (unsigned i = 0; i < ncbs; i++) {
        mod_cb cb = cbs[i];
        if (cb.kind != CB_INPUT) continue;
        current = cb.owner;
        cb.fn.input(pads, ports, cb.user);
    }
    current = save;
    for (int i = 0; i < ports; i++) {
        host[i].connected = pads[i].connected;
        host[i].buttons = pads[i].buttons;
        host[i].lx = pads[i].lx; host[i].ly = pads[i].ly;
        host[i].rx = pads[i].rx; host[i].ry = pads[i].ry;
        host[i].l2 = pads[i].l2; host[i].r2 = pads[i].r2;
    }
}

static int api_patch(uint32_t addr, const void *bytes, const void *original,
                     uint32_t len, int when) {
    if (!on_ee("patch")) return -1;
    return ps2_patch_add(addr, (const u8 *)bytes, (const u8 *)original, len,
                         when, cur_priority(), cur_id(), "api");
}
static int api_unpatch(int handle) {
    return on_ee("unpatch") ? ps2_patch_remove(handle) : -1;
}
static int api_param_set(const char *name, const char *value) {
    if (!on_ee("param_set")) return -1;
    return ps2_params_set(name, value, cur_priority(), cur_id());
}

static void store_path(const mod_api *m, char *out, size_t cap) {
    snprintf(out, cap, "saves/mods/%s.txt", m->id);
}

static void store_put(mod_api *m, const char *key, const char *val) {
    for (unsigned i = 0; i < m->nstore; i++) {
        if (strcmp(m->store[i].key, key)) continue;
        free(m->store[i].val);
        if (!val) {
            free(m->store[i].key);
            m->store[i] = m->store[--m->nstore];
        } else {
            m->store[i].val = strdup(val);
            if (!m->store[i].val) ps2_fatal("mod: out of memory storing a value");
        }
        return;
    }
    if (!val) return;
    if (m->nstore == m->cstore) {
        store_kv *grown;
        m->cstore = m->cstore ? m->cstore * 2u : 16u;
        grown = (store_kv *)realloc(m->store, m->cstore * sizeof *m->store);
        if (!grown) ps2_fatal("mod: out of memory storing a value");
        m->store = grown;
    }
    m->store[m->nstore].key = strdup(key);
    m->store[m->nstore].val = strdup(val);
    if (!m->store[m->nstore].key || !m->store[m->nstore].val)
        ps2_fatal("mod: out of memory storing a value");
    m->nstore++;
}

static void unescape(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s != '\\' || !s[1]) { *o++ = *s; continue; }
        s++;
        *o++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s;
    }
    *o = 0;
}

static char *read_line(FILE *fp) {
    size_t n = 0, cap = 256;
    char *line = (char *)malloc(cap);
    int c;
    if (!line) ps2_fatal("mod: out of memory reading storage");
    while ((c = fgetc(fp)) != EOF && c != '\n') {
        if (n + 2 > cap) {
            char *grown = (char *)realloc(line, cap *= 2);
            if (!grown) ps2_fatal("mod: out of memory reading storage");
            line = grown;
        }
        line[n++] = (char)c;
    }
    if (c == EOF && n == 0) {
        free(line);
        return NULL;
    }
    if (n && line[n - 1] == '\r') n--;
    line[n] = 0;
    return line;
}

static void store_load(mod_api *m) {
    char path[160], *line;
    FILE *fp;
    if (m->store_loaded) return;
    m->store_loaded = 1;
    store_path(m, path, sizeof path);
    fp = fopen(path, "r");
    if (!fp) return;
    while ((line = read_line(fp)) != NULL) {
        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = 0;
            unescape(line);
            unescape(tab + 1);
            store_put(m, line, tab + 1);
        }
        free(line);
    }
    fclose(fp);
}

static void escape_to(FILE *fp, const char *s) {
    for (; *s; s++) {
        if (*s == '\\') fputs("\\\\", fp);
        else if (*s == '\n') fputs("\\n", fp);
        else if (*s == '\t') fputs("\\t", fp);
        else fputc(*s, fp);
    }
}

static void store_flush(mod_api *m) {
    char path[160], tmp[176];
    FILE *fp;
    if (!m->store_dirty) return;
    m->store_dirty = 0;
    ps2_mkdir_p("saves/mods");
    store_path(m, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    fp = fopen(tmp, "w");
    if (!fp) {
        ps2_log("mod[%s]: cannot write %s", m->id, tmp);
        return;
    }
    for (unsigned i = 0; i < m->nstore; i++) {
        escape_to(fp, m->store[i].key);
        fputc('\t', fp);
        escape_to(fp, m->store[i].val);
        fputc('\n', fp);
    }
    if (fclose(fp) != 0) {
        ps2_log("mod[%s]: writing %s failed", m->id, tmp);
        return;
    }
    remove(path);
    if (rename(tmp, path) != 0) ps2_log("mod[%s]: cannot replace %s", m->id, path);
}

static const char *api_store_get(const char *key) {
    if (!key || !current || !on_ee("store_get")) return NULL;
    store_load(current);
    for (unsigned i = 0; i < current->nstore; i++)
        if (!strcmp(current->store[i].key, key)) return current->store[i].val;
    return NULL;
}

static int api_store_set(const char *key, const char *value) {
    if (!key || !*key || !current || !on_ee("store_set")) return -1;
    if (strchr(key, '\t') || strchr(key, '\n')) {
        ps2_log("mod[%s]: a storage key cannot contain a tab or a newline",
                current->id);
        return -1;
    }
    store_load(current);
    store_put(current, key, value);
    current->store_dirty = 1;
    return 0;
}

void ps2_modapi_field_tick(void) {
    mod_api *save = current;
    fields_elapsed++;
    ps2_patch_field();
    for (unsigned i = 0; i < ncbs; i++) {
        mod_cb cb = cbs[i];
        if (cb.kind == CB_FIELD) {
            current = cb.owner;
            cb.fn.field(cb.user);
        } else if (cb.kind == CB_TIMER && fields_elapsed >= cb.due) {
            if (cb.repeat) {
                cbs[i].due = fields_elapsed + cb.period;
            } else {
                counts[CB_TIMER]--;
                cbs[i].kind = CB_FREE;
            }
            current = cb.owner;
            cb.fn.field(cb.user);
        }
    }
    current = save;
    if (!(fields_elapsed % 600u))
        for (unsigned i = 0; i < napis; i++) store_flush(apis[i]);
}

void ps2_modapi_shutdown(void) {
    static int done;
    mod_api *save = current;
    if (done) return;
    done = 1;
    if (ps2_kernel_on_ee_thread()) {
        for (unsigned i = 0; i < ncbs; i++) {
            mod_cb cb = cbs[i];
            if (cb.kind != CB_SHUTDOWN) continue;
            current = cb.owner;
            cb.fn.field(cb.user);
        }
    } else if (counts[CB_SHUTDOWN]) {
        ps2_log("mod: the run ended on a thread without the EE, so %u shutdown "
                "callback(s) did not run", counts[CB_SHUTDOWN]);
    }
    current = save;
    for (unsigned i = 0; i < napis; i++) store_flush(apis[i]);
}

static uint32_t api_symbol(const char *name) {
    return ps2_symbol_find(name);
}
static const char *api_symbol_name(uint32_t addr) {
    return ps2_symbol_name(addr);
}

static const char *api_config(const char *key) {
    if (!key || !current || !current->config_get) return NULL;
    return current->config_get(current->id, key);
}

static int64_t api_file_size(const char *path) {
    return path ? ps2_vfs_stat(path) : -1;
}

static int64_t api_file_read(const char *path, uint64_t pos, void *dst,
                             uint32_t len) {
    if (!path || (len && !dst)) return -1;
    return ps2_vfs_read_path(path, pos, len, dst);
}

static int64_t api_file_to_guest(const char *path, uint64_t pos,
                                 uint32_t addr, uint32_t len) {
    u8 *buf;
    s64 got;
    if (!path || !on_ee("file_to_guest")) return -1;
    if (len > PS2_RAM_SIZE) len = PS2_RAM_SIZE;
    buf = (u8 *)malloc(len ? len : 1u);
    if (!buf) return -1;
    got = ps2_vfs_read_path(path, pos, len, buf);
    if (got > 0) mem_put(addr, buf, (u32)got);
    free(buf);
    return got;
}

const ac5_api *ps2_modapi_for(const char *mod_id, int priority,
                              const char *(*config_get)(const char *mod_id,
                                                        const char *key)) {
    mod_api *m;
    if (napis == capis) {
        mod_api **grown;
        capis = capis ? capis * 2u : 8u;
        grown = (mod_api **)realloc(apis, capis * sizeof *apis);
        if (!grown) ps2_fatal("mod: out of memory handing out a mod api");
        apis = grown;
    }
    m = (mod_api *)calloc(1, sizeof *m);
    if (!m) ps2_fatal("mod: out of memory handing out a mod api");
    apis[napis++] = m;
    snprintf(m->id, sizeof m->id, "%s", mod_id ? mod_id : "?");
    m->config_get = config_get;

    m->api.abi_version = AC5_ABI_VERSION;
    m->api.game_id = "SLUS-20851";
    m->api.mod_id = m->id;
    m->api.priority = priority;
    m->api.log = api_log;
    m->api.read8 = api_read8;
    m->api.read16 = api_read16;
    m->api.read32 = api_read32;
    m->api.read_float = api_read_float;
    m->api.write8 = api_write8;
    m->api.write16 = api_write16;
    m->api.write32 = api_write32;
    m->api.write_float = api_write_float;
    m->api.read_bytes = api_read_bytes;
    m->api.write_bytes = api_write_bytes;
    m->api.arg = api_arg;
    m->api.set_return = api_set_return;
    m->api.reg = api_reg;
    m->api.set_reg = api_set_reg;
    m->api.hook_before = api_hook_before;
    m->api.hook_replace = api_hook_replace;
    m->api.call_original = api_call_original;
    m->api.redirect = api_redirect;
    m->api.call_guest = api_call_guest;
    m->api.on_field = api_on_field;
    m->api.symbol = api_symbol;
    m->api.symbol_name = api_symbol_name;
    m->api.config = api_config;
    m->api.file_size = api_file_size;
    m->api.file_read = api_file_read;
    m->api.file_to_guest = api_file_to_guest;
    m->api.hook_after = api_hook_after;
    m->api.unhook = api_unhook;
    m->api.entry_arg = api_entry_arg;
    m->api.return_value = api_return_value;
    m->api.return_float = api_return_float;
    m->api.set_return_float = api_set_return_float;
    m->api.reg_float = api_reg_float;
    m->api.set_reg_float = api_set_reg_float;
    m->api.on_frame = api_on_frame;
    m->api.on_scene = api_on_scene;
    m->api.timer = api_timer;
    m->api.on_shutdown = api_on_shutdown;
    m->api.cancel = api_cancel;
    m->api.pad = api_pad;
    m->api.on_input = api_on_input;
    m->api.patch = api_patch;
    m->api.unpatch = api_unpatch;
    m->api.param_set = api_param_set;
    m->api.store_get = api_store_get;
    m->api.store_set = api_store_set;
    return &m->api;
}

void ps2_modapi_report(void) {
    for (unsigned i = 0; i < napis; i++)
        if (apis[i]->refused)
            ps2_log("   %s: %llu call(s) refused for arriving on a thread "
                    "without the EE", apis[i]->id,
                    (unsigned long long)apis[i]->refused);
    if (events_hooked)
        ps2_log("   events: %llu frame(s) seen, %u scene machine(s); "
                "callbacks: %u frame, %u scene, %u input, %u timer, %u field",
                (unsigned long long)frames_seen, nmachines, counts[CB_FRAME],
                counts[CB_SCENE], counts[CB_INPUT], counts[CB_TIMER],
                counts[CB_FIELD]);
}
