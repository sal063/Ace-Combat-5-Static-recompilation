#include "ps2_runtime.h"
#include "ps2_hle.h"
#include "ps2_hook.h"
#include "ps2_params.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T_S8, T_U8, T_S16, T_U16, T_S32, T_INT, T_FLOAT, T_STRING };

static const struct { u32 addr; int type; const char *what; } getters[] = {
    { 0x00324568u, T_S8,     "s8"     },
    { 0x00324688u, T_U8,     "u8"     },
    { 0x003247B0u, T_S16,    "s16"    },
    { 0x00324910u, T_U16,    "u16"    },
    { 0x00324A68u, T_S32,    "s32"    },
    { 0x00324BC0u, T_INT,    "int"    },
    { 0x00324D10u, T_FLOAT,  "float"  },
    { 0x00324E68u, T_STRING, "string" },
};
#define NGETTERS (sizeof getters / sizeof getters[0])
#define LIST_STREAMING 1228u

typedef struct {
    char *name, *value;
    int priority;
    const char *owner;
    u64 hits;
    int said_bad;
} param_ov;

static param_ov *ovs;
static unsigned nov, cov;
static int hooked, trace, trace_checked;
static u64 lookups;

static char **seen;
static unsigned nseen, cseen;

static const char *who(const char *owner) { return owner ? owner : "?"; }

static int find(const char *name) {
    for (unsigned i = 0; i < nov; i++)
        if (!strcmp(ovs[i].name, name)) return (int)i;
    return -1;
}

static void trace_name(unsigned gi, const char *name, ps2_ctx *ctx, u32 dst) {
    int type = getters[gi].type;
    char value[160];
    for (unsigned i = 0; i < nseen; i++)
        if (!strcmp(seen[i], name)) return;
    if (nseen == cseen) {
        char **grown;
        cseen = cseen ? cseen * 2u : 256u;
        grown = (char **)realloc(seen, cseen * sizeof *seen);
        if (!grown) return;
        seen = grown;
    }
    seen[nseen] = strdup(name);
    if (!seen[nseen]) return;
    nseen++;
    switch (type) {
    case T_FLOAT:
        snprintf(value, sizeof value, "%g", (double)ctx->f[0].f);
        break;
    case T_STRING: {
        char s[128];
        ps2_get_str(dst, s, sizeof s);
        snprintf(value, sizeof value, "\"%s\"", s);
        break;
    }
    case T_U8: case T_U16:
        snprintf(value, sizeof value, "%u", ctx->r[2].uw[0]);
        break;
    default:
        snprintf(value, sizeof value, "%d", ctx->r[2].sw[0]);
        break;
    }
    ps2_log("param: %s (%s) = %s", name, getters[gi].what, value);
}

static int getter_after(ps2_ctx *ctx, void *user) {
    unsigned gi = (unsigned)(uintptr_t)user;
    int type = getters[gi].type, ok, idx;
    u32 list = ps2_hook_entry_arg(0, &ok);
    u32 name_addr = ps2_hook_entry_arg(1, &ok);
    u32 dst = ps2_hook_entry_arg(2, &ok);
    char name[128];
    param_ov *ov;
    if (!ok || !list || ps2_r32(list + LIST_STREAMING)) return 0;
    ps2_get_str(name_addr, name, sizeof name);
    if (!*name) return 0;
    lookups++;
    if (trace) trace_name(gi, name, ctx, dst);
    if ((idx = find(name)) < 0) return 0;
    ov = &ovs[idx];
    if (type == T_FLOAT) {
        char *end;
        double v = strtod(ov->value, &end);
        if (end != ov->value && !*end) {
            ctx->f[0].f = (float)v;
            ov->hits++;
            return 0;
        }
    } else if (type == T_STRING) {
        size_t n = strlen(ov->value);
        if (n < 128) {
            for (size_t i = 0; i <= n; i++) ps2_w8(dst + (u32)i, (u8)ov->value[i]);
            ov->hits++;
            return 0;
        }
    } else {
        char *end;
        long long v = strtoll(ov->value, &end, 0);
        if (end != ov->value && !*end) {
            s64 r;
            switch (type) {
            case T_S8:  r = (s8)v; break;
            case T_U8:  r = (u8)v; break;
            case T_S16: r = (s16)v; break;
            case T_U16: r = (u16)v; break;
            default:    r = (s32)v; break;
            }
            ctx->r[2].sd[0] = r;
            ov->hits++;
            return 0;
        }
    }
    if (!ov->said_bad) {
        ov->said_bad = 1;
        ps2_log("param: the game asks for %s as %s, and %s's value '%s' is not "
                "one; the game's own value is kept", name, getters[gi].what,
                who(ov->owner), ov->value);
    }
    return 0;
}

void ps2_params_start(void) {
    if (!trace_checked) {
        const char *e = getenv("PS2_PARAM_TRACE");
        trace_checked = 1;
        trace = e && *e && *e != '0';
    }
    if (hooked || (!nov && !trace)) return;
    hooked = 1;
    for (unsigned i = 0; i < NGETTERS; i++) {
        if (ps2_hook_after(getters[i].addr, getter_after, (void *)(uintptr_t)i,
                           INT_MAX, "mod host (params)") < 0) {
            ps2_log("param: CTxtio's %s getter could not be hooked; overrides "
                    "of that type will not apply", getters[i].what);
        }
    }
    ps2_log("param: %u override(s)%s", nov, trace ? ", tracing every name the "
            "game asks for" : "");
}

int ps2_params_set(const char *name, const char *value, int priority,
                   const char *owner) {
    int idx;
    if (!name || !*name) return -1;
    idx = find(name);
    if (!value) {
        if (idx < 0 || strcmp(who(ovs[idx].owner), who(owner))) return -1;
        free(ovs[idx].name);
        free(ovs[idx].value);
        ovs[idx] = ovs[--nov];
        return 0;
    }
    if (idx >= 0) {
        param_ov *ov = &ovs[idx];
        if (strcmp(who(ov->owner), who(owner))) {
            if (ov->priority >= priority) {
                ps2_log("param: conflict on %s -- %s (priority %d) keeps it, so "
                        "%s (priority %d) does not set it", name, who(ov->owner),
                        ov->priority, who(owner), priority);
                return -1;
            }
            ps2_log("param: conflict on %s -- %s (priority %d) takes it from %s "
                    "(priority %d)", name, who(owner), priority, who(ov->owner),
                    ov->priority);
        }
        free(ov->value);
        ov->value = strdup(value);
        ov->priority = priority;
        ov->owner = owner;
        ov->said_bad = 0;
        if (!ov->value) ps2_fatal("param: out of memory");
        return 0;
    }
    if (nov == cov) {
        param_ov *grown;
        cov = cov ? cov * 2u : 32u;
        grown = (param_ov *)realloc(ovs, cov * sizeof *ovs);
        if (!grown) ps2_fatal("param: out of memory");
        ovs = grown;
    }
    memset(&ovs[nov], 0, sizeof ovs[nov]);
    ovs[nov].name = strdup(name);
    ovs[nov].value = strdup(value);
    ovs[nov].priority = priority;
    ovs[nov].owner = owner;
    if (!ovs[nov].name || !ovs[nov].value) ps2_fatal("param: out of memory");
    nov++;
    if (!hooked && ps2_kernel_on_ee_thread()) ps2_params_start();
    return 0;
}

int ps2_params_load(const char *path, int priority, const char *owner) {
    char line[512];
    unsigned lineno = 0;
    int added = 0;
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    while (fgets(line, sizeof line, fp)) {
        char *s = line, *eq, *name, *val, *e;
        lineno++;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#' || *s == '\r' || *s == '\n') continue;
        eq = strchr(s, '=');
        if (!eq) {
            ps2_log("param: %s:%u: expected name = value", path, lineno);
            continue;
        }
        *eq = 0;
        name = s;
        e = eq;
        while (e > name && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;
        if (*val == '"') {
            char *close = strchr(val + 1, '"');
            if (!close) {
                ps2_log("param: %s:%u: unclosed quote", path, lineno);
                continue;
            }
            *close = 0;
            val++;
        } else {
            char *c = strchr(val, '#');
            if (c) *c = 0;
            e = val + strlen(val);
            while (e > val && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'
                               || e[-1] == '\n'))
                *--e = 0;
        }
        if (!*name || strchr(name, ' ')) {
            ps2_log("param: %s:%u: '%s' is not a parameter name", path, lineno,
                    name);
            continue;
        }
        if (ps2_params_set(name, val, priority, owner) == 0) added++;
    }
    fclose(fp);
    ps2_log("param: %s: %d override(s) from %s", who(owner), added, path);
    return added;
}

void ps2_params_report(void) {
    if (!hooked) return;
    ps2_log("---- params ----");
    ps2_log("   %llu named lookup(s) by the game, %u distinct name(s) traced",
            (unsigned long long)lookups, nseen);
    for (unsigned i = 0; i < nov; i++)
        ps2_log("   %-40s = %-12s %s (priority %d), applied %llu time(s)",
                ovs[i].name, ovs[i].value, who(ovs[i].owner), ovs[i].priority,
                (unsigned long long)ovs[i].hits);
}
