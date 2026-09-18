#include "ps2_runtime.h"
#include "ps2_patch.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_END 0x003992C0u

enum { P_ACTIVE, P_CONFLICT, P_REMOVED };

typedef struct {
    int handle;
    u32 addr, len;
    u8 bytes[PS2_PATCH_MAX_BYTES], original[PS2_PATCH_MAX_BYTES];
    int has_original, when, priority, state, done_once, said_skip;
    const char *owner;
    char origin[200];
    u64 writes, skips;
} patch;

static patch **patches;
static unsigned npatches, cpatches;
static int started, next_handle = 1;
static unsigned refused_code, refused_unmapped;

static const char *who(const char *owner) { return owner ? owner : "?"; }

static int mapped(u32 addr, u32 len) {
    for (u32 a = addr & ~PS2_PAGE_MASK; ; a += PS2_PAGE_SIZE) {
        if (!ps2_pt[a >> PS2_PAGE_BITS]) return 0;
        if (a + PS2_PAGE_SIZE >= addr + len || a + PS2_PAGE_SIZE < a) return 1;
    }
}

static void mem_get(u32 addr, u8 *dst, u32 len) {
    for (u32 i = 0; i < len; i++) {
        u32 a = addr + i;
        dst[i] = ps2_pt[a >> PS2_PAGE_BITS][a & PS2_PAGE_MASK];
    }
}

static void mem_put(u32 addr, const u8 *src, u32 len) {
    for (u32 i = 0; i < len; i++) {
        u32 a = addr + i;
        ps2_pt[a >> PS2_PAGE_BITS][a & PS2_PAGE_MASK] = src[i];
    }
}

static int in_code(u32 addr, u32 len) {
    u32 lo, hi, phys = addr;
    if (addr < 0x02000000u || (addr >= 0x20000000u && addr < 0x22000000u)
        || (addr >= 0x30000000u && addr < 0x32000000u)
        || (addr >= 0x80000000u && addr < 0x82000000u)
        || (addr >= 0xA0000000u && addr < 0xA2000000u))
        phys = addr & 0x01FFFFFFu;
    else
        return 0;
    ps2_text_bounds(&lo, &hi);
    if (hi < CODE_END) hi = CODE_END;
    return phys < hi && phys + len > lo;
}

static void hex_bytes(const u8 *b, u32 len, char *out, size_t cap) {
    size_t at = 0;
    for (u32 i = len; i-- > 0 && at + 3 < cap;)
        at += (size_t)snprintf(out + at, cap - at, "%02X", b[i]);
    if (at + 3 >= cap && len) snprintf(out + cap - 4, 4, "...");
}

static void apply(patch *p) {
    u8 cur[PS2_PATCH_MAX_BYTES];
    mem_get(p->addr, cur, p->len);
    if (!memcmp(cur, p->bytes, p->len)) return;
    if (p->has_original && memcmp(cur, p->original, p->len)) {
        p->skips++;
        if (!p->said_skip) {
            char have[64], want[64];
            p->said_skip = 1;
            hex_bytes(cur, p->len, have, sizeof have);
            hex_bytes(p->original, p->len, want, sizeof want);
            ps2_log("patch: %s's patch at %08X (%s) finds %s where it expects "
                    "%s, so it is not written (counted from now on)",
                    who(p->owner), p->addr, p->origin, have, want);
        }
        return;
    }
    mem_put(p->addr, p->bytes, p->len);
    p->writes++;
}

int ps2_patch_add(u32 addr, const u8 *bytes, const u8 *original, u32 len,
                  int when, int priority, const char *owner,
                  const char *origin) {
    patch *p;
    if (!bytes || len == 0 || len > PS2_PATCH_MAX_BYTES || when < 0 || when > 2) {
        ps2_log("patch: %s's patch at %08X (%s) is malformed", who(owner), addr,
                origin ? origin : "?");
        return -1;
    }
    if (in_code(addr, len)) {
        refused_code++;
        ps2_log("patch: %s's patch at %08X (%s) is in the game's code, which "
                "was recompiled ahead of time -- writing it would change "
                "nothing.  Change the function's behaviour with a hook "
                "instead.", who(owner), addr, origin ? origin : "?");
        return -1;
    }
    if (addr + len < addr || !mapped(addr, len)) {
        refused_unmapped++;
        ps2_log("patch: %s's patch at %08X (%s) is not in the game's memory "
                "(RAM, scratchpad or VU memory); refused", who(owner), addr,
                origin ? origin : "?");
        return -1;
    }
    for (unsigned i = 0; i < npatches; i++) {
        patch *q = patches[i];
        if (q->state != P_ACTIVE || q->addr >= addr + len || addr >= q->addr + q->len)
            continue;
        if (q->owner == owner || (q->owner && owner && !strcmp(q->owner, owner))) {
            const char *qs = strchr(q->origin, '['), *ns = origin ? strchr(origin, '[') : NULL;
            if (qs && ns && strcmp(qs, ns))
                ps2_log("patch: %s patches %08X in two sections, %s and %s; "
                        "the later one wins -- keep only the section you want",
                        who(owner), addr, qs, ns);
            continue;
        }
        if (q->priority >= priority) {
            ps2_log("patch: conflict at %08X -- %s (priority %d, %s) keeps it, "
                    "so %s's patch (priority %d, %s) is off", addr,
                    who(q->owner), q->priority, q->origin, who(owner), priority,
                    origin ? origin : "?");
            return -1;
        }
        ps2_log("patch: conflict at %08X -- %s (priority %d, %s) takes it "
                "from %s (priority %d, %s)", addr, who(owner), priority,
                origin ? origin : "?", who(q->owner), q->priority, q->origin);
        q->state = P_CONFLICT;
    }
    if (npatches == cpatches) {
        patch **grown;
        cpatches = cpatches ? cpatches * 2u : 32u;
        grown = (patch **)realloc(patches, cpatches * sizeof *patches);
        if (!grown) ps2_fatal("patch: out of memory");
        patches = grown;
    }
    p = (patch *)calloc(1, sizeof *p);
    if (!p) ps2_fatal("patch: out of memory");
    p->handle = next_handle++;
    p->addr = addr;
    p->len = len;
    memcpy(p->bytes, bytes, len);
    if (original) {
        memcpy(p->original, original, len);
        p->has_original = 1;
    }
    p->when = when;
    p->priority = priority;
    p->owner = owner;
    snprintf(p->origin, sizeof p->origin, "%s", origin ? origin : "api");
    patches[npatches++] = p;
    if (started && when != PS2_PATCH_ALWAYS) {
        apply(p);
        p->done_once = 1;
    }
    return p->handle;
}

int ps2_patch_remove(int handle) {
    for (unsigned i = 0; i < npatches; i++) {
        patch *p = patches[i];
        u8 cur[PS2_PATCH_MAX_BYTES];
        if (p->handle != handle || p->state == P_REMOVED) continue;
        if (p->state == P_ACTIVE && p->has_original) {
            mem_get(p->addr, cur, p->len);
            if (!memcmp(cur, p->bytes, p->len)) mem_put(p->addr, p->original, p->len);
        }
        p->state = P_REMOVED;
        ps2_log("patch: %s's patch at %08X (%s) removed%s", who(p->owner),
                p->addr, p->origin, p->has_original
                    ? ", original bytes put back" : "");
        return 0;
    }
    return -1;
}

void ps2_patch_start(void) {
    unsigned once = 0, always = 0;
    started = 1;
    for (unsigned i = 0; i < npatches; i++) {
        patch *p = patches[i];
        if (p->state != P_ACTIVE) continue;
        if (p->when != PS2_PATCH_ALWAYS && !p->done_once) {
            apply(p);
            p->done_once = 1;
            once++;
        }
        if (p->when != PS2_PATCH_ONCE) always++;
    }
    if (npatches)
        ps2_log("patch: %u patch(es) applied at start, %u applied every field",
                once, always);
}

void ps2_patch_field(void) {
    for (unsigned i = 0; i < npatches; i++) {
        patch *p = patches[i];
        if (p->state == P_ACTIVE && p->when != PS2_PATCH_ONCE) apply(p);
    }
}

static char *trim(char *s) {
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return s;
}

static int parse_hex(const char *s, u64 *out) {
    char *end;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!isxdigit((unsigned char)*s)) return -1;
    *out = strtoull(s, &end, 16);
    return *trim(end) ? -1 : 0;
}

int ps2_patch_load_pnach(const char *path, int priority, const char *owner) {
    static const char *const quiet_keys[] = {
        "author", "comment", "description", "gametitle", "gsaspectratio",
        "gsinterlacemode", "gsnativeres", "dpadskipframes", NULL };
    char line[1024], section[96] = "", origin[200];
    FILE *fp = fopen(path, "r");
    unsigned lineno = 0;
    int added = 0, skip_section = 0, said_keys = 0;
    if (!fp) {
        ps2_log("patch: cannot open %s", path);
        return 0;
    }
    while (fgets(line, sizeof line, fp)) {
        char *s = line, *eq, *key, *val, *f[5];
        char *c;
        int nf = 0, when, width = 0, big = 0;
        u64 addr, value = 0;
        u8 bytes[PS2_PATCH_MAX_BYTES];
        lineno++;
        if (lineno == 1 && !memcmp(s, "\xEF\xBB\xBF", 3)) s += 3;
        if ((c = strstr(s, "//")) != NULL) *c = 0;
        s = trim(s);
        if (!*s) continue;
        if (*s == '[') {
            char *end = strchr(s, ']');
            snprintf(section, sizeof section, "%.*s",
                     (int)(end ? end - s - 1 : (long)strlen(s) - 1), s + 1);
            skip_section = 0;
            continue;
        }
        eq = strchr(s, '=');
        if (!eq) {
            ps2_log("patch: %s:%u is not key=value; ignored", path, lineno);
            continue;
        }
        *eq = 0;
        key = trim(s);
        val = trim(eq + 1);
        for (char *k = key; *k; k++) *k = (char)tolower((unsigned char)*k);
        if (strcmp(key, "patch")) {
            int quiet = 0;
            for (const char *const *q = quiet_keys; *q; q++)
                if (!strcmp(key, *q)) quiet = 1;
            if (!quiet && !said_keys++)
                ps2_log("patch: %s:%u: '%s' is not something this runtime "
                        "applies; ignored (and any like it in this file)",
                        path, lineno, key);
            continue;
        }
        if (skip_section) continue;
        for (char *tok = val; nf < 5; nf++) {
            f[nf] = tok;
            tok = nf < 4 ? strchr(tok, ',') : NULL;
            if (!tok) { nf++; break; }
            *tok++ = 0;
        }
        snprintf(origin, sizeof origin, "%s:%u%s%s%s", path, lineno,
                 *section ? " [" : "", section, *section ? "]" : "");
        if (nf != 5) {
            ps2_log("patch: %s needs five fields (place,cpu,address,type,value)",
                    origin);
            continue;
        }
        for (int i = 0; i < 5; i++) f[i] = trim(f[i]);
        when = (int)strtol(f[0], NULL, 10);
        if (strlen(f[0]) != 1 || when < 0 || when > 2) {
            ps2_log("patch: %s: place '%s' is not 0, 1 or 2", origin, f[0]);
            continue;
        }
        if (strcmp(f[1], "EE")) {
            ps2_log("patch: %s patches the %s; only EE memory is the game's",
                    origin, f[1]);
            continue;
        }
        if (parse_hex(f[2], &addr) != 0 || addr > 0xFFFFFFFFull) {
            ps2_log("patch: %s: address '%s' is not hexadecimal", origin, f[2]);
            continue;
        }
        if (!strcmp(f[3], "bytes")) {
            size_t n = strlen(f[4]);
            if (n == 0 || n % 2 || n / 2 > PS2_PATCH_MAX_BYTES) {
                ps2_log("patch: %s: a bytes value needs an even number of hex "
                        "digits, at most %d bytes", origin, PS2_PATCH_MAX_BYTES);
                continue;
            }
            width = (int)(n / 2);
            for (int i = 0; i < width; i++) {
                char hx[3] = { f[4][2 * i], f[4][2 * i + 1], 0 };
                if (!isxdigit((unsigned char)hx[0]) || !isxdigit((unsigned char)hx[1])) {
                    width = -1;
                    break;
                }
                bytes[i] = (u8)strtoul(hx, NULL, 16);
            }
            if (width < 0) {
                ps2_log("patch: %s: '%s' is not hexadecimal", origin, f[4]);
                continue;
            }
        } else {
            if (parse_hex(f[4], &value) != 0) {
                ps2_log("patch: %s: value '%s' is not hexadecimal", origin, f[4]);
                continue;
            }
            if (!strcmp(f[3], "byte")) width = 1;
            else if (!strcmp(f[3], "short")) width = 2;
            else if (!strcmp(f[3], "word")) width = 4;
            else if (!strcmp(f[3], "double")) width = 8;
            else if (!strcmp(f[3], "beshort")) { width = 2; big = 1; }
            else if (!strcmp(f[3], "beword")) { width = 4; big = 1; }
            else if (!strcmp(f[3], "bedouble")) { width = 8; big = 1; }
            else if (!strcmp(f[3], "extended")) {
                unsigned code = (unsigned)(addr >> 28);
                if (code > 2) {
                    ps2_log("patch: %s: extended code type %X is a conditional "
                            "or computed code, which is not supported; the "
                            "rest of this section is skipped, because the "
                            "lines after it may depend on it", origin, code);
                    skip_section = 1;
                    continue;
                }
                width = code == 0 ? 1 : code == 1 ? 2 : 4;
                addr &= 0x0FFFFFFFull;
            } else {
                ps2_log("patch: %s: type '%s' is not one PCSX2 defines", origin,
                        f[3]);
                continue;
            }
            for (int i = 0; i < width; i++)
                bytes[big ? width - 1 - i : i] = (u8)(value >> (8 * i));
        }
        if (ps2_patch_add((u32)addr, bytes, NULL, (u32)width, when, priority,
                          owner, origin) > 0)
            added++;
    }
    fclose(fp);
    ps2_log("patch: %s: %d patch(es) from %s", who(owner), added, path);
    return added;
}

void ps2_patch_report(void) {
    unsigned active = 0, conflict = 0, removed = 0;
    if (!npatches && !refused_code && !refused_unmapped) return;
    for (unsigned i = 0; i < npatches; i++) {
        const patch *p = patches[i];
        if (p->state == P_ACTIVE) active++;
        else if (p->state == P_CONFLICT) conflict++;
        else removed++;
    }
    ps2_log("---- patches ----");
    ps2_log("   %u active, %u lost a conflict, %u removed; refused: %u in code, "
            "%u outside memory", active, conflict, removed, refused_code,
            refused_unmapped);
    for (unsigned i = 0; i < npatches; i++) {
        const patch *p = patches[i];
        if (!p->skips) continue;
        ps2_log("   %08X %s (%s): skipped %llu time(s), the memory did not hold "
                "the expected original", p->addr, who(p->owner), p->origin,
                (unsigned long long)p->skips);
    }
}
