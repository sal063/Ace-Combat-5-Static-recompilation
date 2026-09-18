#include "ps2_runtime.h"
#include "ps2_hle.h"
#include "ps2_hook.h"
#include "ps2_mod.h"
#include "ps2_modapi.h"
#include "ps2_params.h"
#include "ps2_patch.h"
#include "ps2_vfs.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#ifdef _WIN32
#include <windows.h>
#endif

typedef struct { char *key, *val; } mod_opt;

typedef struct {
    char id[64];
    char name[96], version[32], author[64];
    int priority;
    int enabled;
    int native;
    int abi;
    char game[32];
    char requires[256];
    char off_why[200];
    char dir[640];
    mod_opt *opts;
    unsigned nopts;
    u32 files;
    u32 natives_ok, natives_failed, patches, params, symbols;
    const ac5_api *api;
} mod_info;

static mod_info *mods;
static u32 nmods, cmods;
static u32 *load_order;
static int mods_off;
static char mod_root[512];

static void opt_set(mod_info *m, const char *key, const char *val) {
    for (unsigned i = 0; i < m->nopts; i++) {
        if (strcmp(m->opts[i].key, key)) continue;
        free(m->opts[i].val);
        m->opts[i].val = strdup(val);
        return;
    }
    m->opts = (mod_opt *)realloc(m->opts, (m->nopts + 1u) * sizeof *m->opts);
    if (!m->opts) ps2_fatal("mod: out of memory reading a manifest");
    m->opts[m->nopts].key = strdup(key);
    m->opts[m->nopts].val = strdup(val);
    if (!m->opts[m->nopts].key || !m->opts[m->nopts].val)
        ps2_fatal("mod: out of memory reading a manifest");
    m->nopts++;
}

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int at_end(const char *p) {
    p = skip_ws(p);
    return !*p || *p == '#' || *p == '\r' || *p == '\n';
}

static const char *read_token(const char *p, char *out, size_t cap,
                              int value, int *quoted) {
    size_t n = 0;
    *quoted = 0;
    if (*p == '"' || *p == '\'') {
        char q = *p++;
        *quoted = 1;
        for (; *p && *p != q && *p != '\n'; p++) {
            char c = *p;
            if (q == '"' && c == '\\') {
                switch (*++p) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                default: return NULL;
                }
            }
            if (n + 1 >= cap) return NULL;
            out[n++] = c;
        }
        if (*p != q) return NULL;
        out[n] = 0;
        return p + 1;
    }
    if (value) {
        const char *end = p;
        while (*end && *end != '#' && *end != '\r' && *end != '\n') end++;
        while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if (end == p || (size_t)(end - p) >= cap) return NULL;
        memcpy(out, p, (size_t)(end - p));
        out[end - p] = 0;
        return end;
    }
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
           || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.') {
        if (n + 1 >= cap) return NULL;
        out[n++] = *p++;
    }
    if (!n) return NULL;
    out[n] = 0;
    return p;
}

static void set_text(char *dst, size_t cap, const char *val, const char *path,
                     unsigned lineno, const char *key) {
    size_t len = strlen(val);
    if (len >= cap) {
        ps2_log("mod: %s line %u: %s is longer than %u characters; shortened",
                path, lineno, key, (unsigned)cap - 1u);
        len = cap - 1u;
    }
    memcpy(dst, val, len);
    dst[len] = 0;
}

static int parse_bool(const char *v, int quoted, int *out) {
    if (quoted) return -1;
    if (!strcmp(v, "true")) { *out = 1; return 0; }
    if (!strcmp(v, "false")) { *out = 0; return 0; }
    return -1;
}

static void manifest_read(mod_info *m) {
    char path[700], line[1024], section[64] = "";
    unsigned lineno = 0;
    FILE *fp;
    int n = snprintf(path, sizeof path, "%s/mod.toml", m->dir);
    if (n < 0 || (size_t)n >= sizeof path) return;
    fp = fopen(path, "r");
    if (!fp) return;
    while (fgets(line, sizeof line, fp)) {
        char key[64], val[512];
        const char *p = line;
        int kq, vq;
        lineno++;
        if (lineno == 1 && !memcmp(p, "\xEF\xBB\xBF", 3)) p += 3;
        p = skip_ws(p);
        if (at_end(p)) continue;
        if (!strchr(line, '\n') && !feof(fp)) {
            int c;
            ps2_log("mod: %s line %u is longer than %u characters; skipped",
                    path, lineno, (unsigned)sizeof line - 1u);
            while ((c = fgetc(fp)) != EOF && c != '\n') {}
            continue;
        }
        if (*p == '[') {
            const char *end = strchr(p, ']');
            size_t len;
            if (!end || !at_end(end + 1)) {
                ps2_log("mod: %s line %u: a [section] header that does not "
                        "close; ignored", path, lineno);
                continue;
            }
            p = skip_ws(p + 1);
            len = (size_t)(end - p);
            while (len && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
            snprintf(section, sizeof section, "%.*s", (int)len, p);
            if (strcmp(section, "config"))
                ps2_log("mod: %s line %u: section [%s] is not one this runtime "
                        "reads; its keys are ignored", path, lineno, section);
            continue;
        }
        p = read_token(p, key, sizeof key, 0, &kq);
        if (p) p = skip_ws(p);
        if (!p || *p != '=') {
            ps2_log("mod: %s line %u: expected key = value", path, lineno);
            continue;
        }
        p = read_token(skip_ws(p + 1), val, sizeof val, 1, &vq);
        if (!p || !at_end(p)) {
            ps2_log("mod: %s line %u: the value of '%s' is malformed (an "
                    "unclosed quote, or text after it)", path, lineno, key);
            continue;
        }
        if (*section && strcmp(section, "config")) continue;
        if (*section) { opt_set(m, key, val); continue; }

        if (!strcmp(key, "name"))
            set_text(m->name, sizeof m->name, val, path, lineno, key);
        else if (!strcmp(key, "version"))
            set_text(m->version, sizeof m->version, val, path, lineno, key);
        else if (!strcmp(key, "author"))
            set_text(m->author, sizeof m->author, val, path, lineno, key);
        else if (!strcmp(key, "priority")) {
            char *end;
            long v = strtol(val, &end, 10);
            if (vq || *end || end == val || v < -1000000 || v > 1000000)
                ps2_log("mod: %s line %u: priority must be a whole number, "
                        "not '%s'; left at %d", path, lineno, val, m->priority);
            else
                m->priority = (int)v;
        } else if (!strcmp(key, "requires")) {
            set_text(m->requires, sizeof m->requires, val, path, lineno, key);
        } else if (!strcmp(key, "game")) {
            set_text(m->game, sizeof m->game, val, path, lineno, key);
        } else if (!strcmp(key, "abi")) {
            char *end;
            long v = strtol(val, &end, 10);
            if (vq || *end || end == val || v < 0)
                ps2_log("mod: %s line %u: abi must be a whole number, not '%s'",
                        path, lineno, val);
            else
                m->abi = (int)v;
        } else if (!strcmp(key, "enabled") || !strcmp(key, "native")) {
            int b;
            if (parse_bool(val, vq, &b) != 0)
                ps2_log("mod: %s line %u: %s must be true or false, not '%s'; "
                        "left unchanged", path, lineno, key, val);
            else if (key[0] == 'e')
                m->enabled = b;
            else
                m->native = b;
        } else {
            opt_set(m, key, val);
        }
    }
    fclose(fp);
}

static int host_size(const char *path, u64 *size, int *is_dir) {
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return -1;
#else
    struct stat st;
    if (stat(path, &st) != 0) return -1;
#endif
    *size = (u64)st.st_size;
    *is_dir = S_ISDIR(st.st_mode);
    return 0;
}

static void scan_files(mod_info *m, const char *dir, const char *prefix,
                       int depth) {
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        char host[1024], rel[512];
        u64 size;
        int is_dir, hn, rn;
        if (e->d_name[0] == '.') continue;
        hn = snprintf(host, sizeof host, "%s/%s", dir, e->d_name);
        rn = snprintf(rel, sizeof rel, "%s%s%s", prefix, *prefix ? "/" : "",
                      e->d_name);
        if (hn < 0 || (size_t)hn >= sizeof host || rn < 0
            || (size_t)rn >= sizeof rel) {
            ps2_log("mod: %s: path too long, skipped: %s/%s", m->id, dir,
                    e->d_name);
            continue;
        }
        if (host_size(host, &size, &is_dir) != 0) continue;
        if (is_dir) {
            if (depth < 16) scan_files(m, host, rel, depth + 1);
            else ps2_log("mod: %s: %s is nested too deeply; skipped", m->id, host);
            continue;
        }
        ps2_vfs_claim(rel, host, size, m->priority, m->id);
        m->files++;
    }
    closedir(d);
}

static int mod_order(const void *a, const void *b) {
    const mod_info *x = (const mod_info *)a, *y = (const mod_info *)b;
    if (x->priority != y->priority) return x->priority < y->priority ? -1 : 1;
    return strcmp(x->id, y->id);
}

static u32 symbols_load(const char *path, const char *owner) {
    char line[512];
    unsigned lineno = 0;
    u32 added = 0, taken = 0;
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    while (fgets(line, sizeof line, fp)) {
        char *p = line, *end, *name, *c;
        unsigned long addr;
        lineno++;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == '\n' || *p == '\r') continue;
        addr = strtoul(p, &end, 16);
        if (end == p || (*end != ' ' && *end != '\t')) {
            ps2_log("mod: %s:%u: expected ADDRESS NAME", path, lineno);
            continue;
        }
        name = end;
        while (*name == ' ' || *name == '\t') name++;
        if ((c = strchr(name, '#')) != NULL) *c = 0;
        end = name + strlen(name);
        while (end > name && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n'
                              || end[-1] == '\r'))
            *--end = 0;
        if (!*name || strchr(name, ' ')) {
            ps2_log("mod: %s:%u: a name cannot be empty or contain spaces", path,
                    lineno);
            continue;
        }
        if (ps2_symbol_add((u32)addr, name) == 0) added++;
        else taken++;
    }
    fclose(fp);
    ps2_log("mod: %u name(s) from %s%s%s%s", added, path, owner ? " (" : "",
            owner ? owner : "", owner ? ")" : "");
    if (taken)
        ps2_log("mod: %u of them name an address that already has a name, and "
                "were not taken", taken);
    return added;
}

static int version_cmp(const char *a, const char *b) {
    while (*a || *b) {
        char *ea, *eb;
        unsigned long x = strtoul(a, &ea, 10), y = strtoul(b, &eb, 10);
        if (x != y) return x < y ? -1 : 1;
        a = (*ea == '.') ? ea + 1 : ea + strlen(ea);
        b = (*eb == '.') ? eb + 1 : eb + strlen(eb);
    }
    return 0;
}

static mod_info *mod_by_id(const char *id) {
    for (u32 i = 0; i < nmods; i++)
        if (!strcmp(mods[i].id, id)) return &mods[i];
    return NULL;
}

static int requires_each(const mod_info *m, int (*fn)(const mod_info *m,
                         const char *id, const char *op, const char *ver,
                         void *u), void *u) {
    char buf[256], *save = NULL, *item;
    int n = 0;
    snprintf(buf, sizeof buf, "%s", m->requires);
    for (item = strtok_r(buf, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
        char id[64] = "", op[3] = "", ver[32] = "";
        int got = sscanf(item, " %63[A-Za-z0-9_.-] %2[<>=] %31[0-9.]", id, op, ver);
        if (got != 1 && got != 3) return -1;
        n++;
        if (fn && fn(m, id, op, ver, u) != 0) return n;
    }
    return n;
}

static int require_check(const mod_info *m, const char *id, const char *op,
                         const char *ver, void *u) {
    mod_info *dep = mod_by_id(id);
    char *why = (char *)u;
    int c;
    if (!dep || !dep->enabled) {
        snprintf(why, 200, "it requires '%s', which is %s", id,
                 dep ? "switched off" : "not installed");
        return 1;
    }
    if (!*op) return 0;
    c = version_cmp(dep->version, ver);
    if ((!strcmp(op, ">=") && c >= 0) || (!strcmp(op, ">") && c > 0)
        || (!strcmp(op, "=") && c == 0) || (!strcmp(op, "<=") && c <= 0)
        || (!strcmp(op, "<") && c < 0))
        return 0;
    snprintf(why, 200, "it requires %s %s %s, and %s is version %s", id, op, ver,
             id, dep->version[0] ? dep->version : "(none given)");
    return 1;
}

static void switch_off(mod_info *m, const char *why) {
    m->enabled = 0;
    snprintf(m->off_why, sizeof m->off_why, "%s", why);
    ps2_log("mod: %s is switched off: %s", m->id, why);
}

static void requirements_resolve(void) {
    int changed = 1;
    for (u32 i = 0; i < nmods; i++) {
        mod_info *m = &mods[i];
        if (!m->enabled) continue;
        if (m->abi > AC5_ABI_VERSION) {
            char why[200];
            snprintf(why, sizeof why, "it needs mod ABI %d, and this runtime "
                     "has %d", m->abi, AC5_ABI_VERSION);
            switch_off(m, why);
        } else if (m->game[0] && strcmp(m->game, "SLUS-20851")) {
            char why[200];
            snprintf(why, sizeof why, "it is for %s, and this is SLUS-20851",
                     m->game);
            switch_off(m, why);
        } else if (requires_each(m, NULL, NULL) < 0) {
            switch_off(m, "its requires line is malformed -- write it as "
                          "\"id\" or \"id >= 1.2\", separated by commas");
        }
    }
    while (changed) {
        changed = 0;
        for (u32 i = 0; i < nmods; i++) {
            char why[200] = "";
            if (!mods[i].enabled || !mods[i].requires[0]) continue;
            requires_each(&mods[i], require_check, why);
            if (*why) {
                switch_off(&mods[i], why);
                changed = 1;
            }
        }
    }
}

static int require_unplaced(const mod_info *m, const char *id, const char *op,
                            const char *ver, void *u) {
    const u8 *placed = (const u8 *)u;
    mod_info *dep = mod_by_id(id);
    (void)m; (void)op; (void)ver;
    return (dep && !placed[dep - mods]) ? 1 : 0;
}

static void load_order_build(void) {
    u8 *placed = (u8 *)calloc(nmods ? nmods : 1u, 1);
    u32 n = 0;
    load_order = (u32 *)malloc((nmods ? nmods : 1u) * sizeof *load_order);
    if (!placed || !load_order) ps2_fatal("mod: out of memory ordering mods");
    for (u32 i = 0; i < nmods; i++)
        if (!mods[i].enabled) placed[i] = 1;
    for (;;) {
        int progress = 0;
        for (u32 i = 0; i < nmods; i++) {
            u8 pending = 0;
            if (placed[i]) continue;
            if (mods[i].requires[0]) {
                char buf[256], *save = NULL, *item;
                snprintf(buf, sizeof buf, "%s", mods[i].requires);
                for (item = strtok_r(buf, ",", &save); item;
                     item = strtok_r(NULL, ",", &save)) {
                    char id[64] = "";
                    if (sscanf(item, " %63[A-Za-z0-9_.-]", id) == 1
                        && require_unplaced(&mods[i], id, "", "", placed))
                        pending = 1;
                }
            }
            if (pending) continue;
            placed[i] = 1;
            load_order[n++] = i;
            progress = 1;
        }
        if (!progress) break;
    }
    for (u32 i = 0; i < nmods; i++) {
        if (placed[i]) continue;
        switch_off(&mods[i], "its requirements form a cycle, so no load order "
                             "satisfies them");
    }
    for (u32 i = 0; i < nmods; i++)
        if (!mods[i].enabled) load_order[n++] = i;
    free(placed);
}

void ps2_mod_init(void) {
    const char *root = getenv("PS2_MOD_DIR");
    DIR *d;
    struct dirent *e;
    u32 enabled = 0, files = 0;
    symbols_load("config/game_symbols.txt", NULL);
    if (getenv("PS2_NO_MODS")) {
        mods_off = 1;
        ps2_log("mod: disabled by PS2_NO_MODS");
        return;
    }
    snprintf(mod_root, sizeof mod_root, "%s", (root && *root) ? root : "mods");
    d = opendir(mod_root);
    if (!d) {
        ps2_log("mod: no mods directory at '%s'", mod_root);
        return;
    }
    while ((e = readdir(d)) != NULL) {
        char sub[1024];
        u64 size;
        int is_dir, sn;
        mod_info *m;
        if (e->d_name[0] == '.') continue;
        sn = snprintf(sub, sizeof sub, "%s/%s", mod_root, e->d_name);
        if (sn < 0 || (size_t)sn >= sizeof sub) continue;
        if (host_size(sub, &size, &is_dir) != 0 || !is_dir) continue;
        if (strlen(e->d_name) >= sizeof m->id || strlen(sub) >= sizeof m->dir) {
            ps2_log("mod: path too long, skipped: %s", sub);
            continue;
        }
        if (nmods == cmods) {
            cmods = cmods ? cmods * 2u : 8u;
            mods = (mod_info *)realloc(mods, cmods * sizeof *mods);
            if (!mods) ps2_fatal("mod: out of memory listing mods");
        }
        m = &mods[nmods++];
        memset(m, 0, sizeof *m);
        snprintf(m->id, sizeof m->id, "%s", e->d_name);
        snprintf(m->dir, sizeof m->dir, "%s", sub);
        m->enabled = 1;
        manifest_read(m);
    }
    closedir(d);

    if (nmods > 1) qsort(mods, nmods, sizeof *mods, mod_order);
    for (u32 i = 0; i < nmods; i++)
        if (!mods[i].enabled) {
            snprintf(mods[i].off_why, sizeof mods[i].off_why,
                     "its manifest says enabled = false");
            ps2_log("mod: %s is switched off by its manifest", mods[i].id);
        }
    requirements_resolve();
    load_order_build();
    for (u32 i = 0; i < nmods; i++) {
        char fdir[700];
        int fn;
        if (!mods[i].enabled) continue;
        enabled++;
        fn = snprintf(fdir, sizeof fdir, "%s/files", mods[i].dir);
        if (fn > 0 && (size_t)fn < sizeof fdir) scan_files(&mods[i], fdir, "", 0);
        files += mods[i].files;
    }
    ps2_log("mod: %u mod(s) in '%s', %u enabled, %u file(s) for the filesystem",
            nmods, mod_root, enabled, files);
}

static const char *mod_config_get(const char *mod_id, const char *key) {
    for (u32 i = 0; i < nmods; i++) {
        if (strcmp(mods[i].id, mod_id)) continue;
        for (unsigned k = 0; k < mods[i].nopts; k++)
            if (!strcmp(mods[i].opts[k].key, key)) return mods[i].opts[k].val;
        return NULL;
    }
    return NULL;
}

unsigned ps2_mod_count(void) { return load_order ? nmods : 0; }

const char *ps2_mod_id_at(unsigned i) {
    return i < ps2_mod_count() ? mods[load_order[i]].id : NULL;
}

const char *ps2_mod_dir_at(unsigned i) {
    return (i < ps2_mod_count() && mods[load_order[i]].enabled)
        ? mods[load_order[i]].dir : NULL;
}

static const ac5_api *api_of(mod_info *m) {
    if (!m->api) m->api = ps2_modapi_for(m->id, m->priority, mod_config_get);
    return m->api;
}

const ac5_api *ps2_mod_api_at(unsigned i) {
    return i < ps2_mod_count() ? api_of(&mods[load_order[i]]) : NULL;
}

#define AC5_ULZ_SETUP  0x00102A78u

static const u32 ulz_setup_code[] = {
    0x8CA20004u, 0x3C0700FFu, 0x34E7FFFFu, 0x24080001u,
};
static u64 archive_served, archive_passed;

static int archive_member_setup(ps2_ctx *ctx, void *user) {
    u32 state = ps2_arg(ctx, 0), dst = ps2_arg(ctx, 2);
    u32 loader = state - 4u, slot, member, count, table, capacity;
    u8 mode;
    (void)user;
    ps2_hook_call_original(AC5_ULZ_SETUP, ctx);
    mode = ps2_r8(loader);
    slot = ps2_r32(loader + 48u);
    if ((mode != 4 && mode != 5) || slot >= 7u) return 1;
    member = ps2_r16(loader + 244u + 2u * slot);
    if (!ps2_vfs_pac_is_changed(member)) {
        archive_passed++;
        return 1;
    }
    count = ps2_r32(loader + 56u);
    table = ps2_r32(loader + 64u);
    capacity = member < count ? ps2_r32(table + 8u * count + 4u * member) : 0;
    if (capacity != ps2_vfs_pac_size(member)) {
        static int said;
        if (!said++)
            ps2_log("mod: the game's DATA.TBL says member %u is %u bytes and "
                    "the filesystem says %u -- the table it holds is not the "
                    "one served, so the member is left to its decompressor",
                    member, capacity, ps2_vfs_pac_size(member));
        return 1;
    }
    if (ps2_vfs_pac_fill_guest(member, dst, capacity) < 0) {
        ps2_log("mod: DATA.PAC member %u could not be built; the game gets it "
                "empty", member);
        for (u32 i = 0; i < capacity; i++) ps2_w8(dst + i, 0);
    }
    ps2_w32(state + 28u, 0);
    archive_served++;
    ps2_log("mod: DATA.PAC member %u (%u bytes) served from the filesystem",
            member, capacity);
    return 1;
}

static void archive_driver_install(void) {
    for (unsigned i = 0; i < sizeof ulz_setup_code / sizeof ulz_setup_code[0]; i++) {
        if (ps2_r32(AC5_ULZ_SETUP + 4u * i) == ulz_setup_code[i]) continue;
        ps2_vfs_pac_revert("the code at 00102A78 is not DATA.PAC's decompressor "
                           "set-up this runtime was written against");
        return;
    }
    if (ps2_hook_replace(AC5_ULZ_SETUP, archive_member_setup, NULL, INT_MAX,
                         "mod host (DATA.PAC)") < 0) {
        ps2_vfs_pac_revert("DATA.PAC's decompressor could not be hooked; the "
                           "hook: line above says why");
        return;
    }
    ps2_log("mod: DATA.PAC's loader reads %u changed member(s) from the "
            "filesystem", ps2_vfs_pac_changed());
}

static int str_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void natives_load(mod_info *m) {
    DIR *d = opendir(m->dir);
    struct dirent *e;
    char *names[64];
    unsigned n = 0;
    if (!d) return;
    while ((e = readdir(d)) != NULL && n < 64) {
        size_t len = strlen(e->d_name);
        if (len < 5) continue;
        if (strcmp(e->d_name + len - 4, ".dll") && strcmp(e->d_name + len - 4, ".DLL"))
            continue;
        names[n] = strdup(e->d_name);
        if (names[n]) n++;
    }
    closedir(d);
    if (!n) return;
    qsort(names, n, sizeof *names, str_cmp);
    if (!m->native) {
        ps2_log("mod: %s ships native code (%s%s) but its mod.toml does not "
                "say native = true, so it is not loaded", m->id, names[0],
                n > 1 ? ", ..." : "");
        for (unsigned i = 0; i < n; i++) free(names[i]);
        return;
    }
    for (unsigned i = 0; i < n; i++) {
#ifdef _WIN32
        char rel[1024], full[1024];
        HMODULE h;
        ac5_mod_main_fn entry;
        const ac5_api *api = api_of(m), *prev;
        int rc;
        snprintf(rel, sizeof rel, "%s/%s", m->dir, names[i]);
        if (!_fullpath(full, rel, sizeof full)) {
            m->natives_failed++;
            free(names[i]);
            continue;
        }
        h = LoadLibraryExA(full, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!h) {
            ps2_log("mod: %s: cannot load %s (error %lu)", m->id, names[i],
                    (unsigned long)GetLastError());
            m->natives_failed++;
            free(names[i]);
            continue;
        }
        entry = (ac5_mod_main_fn)(void (*)(void))GetProcAddress(h, "ac5_mod_main");
        if (!entry) {
            ps2_log("mod: %s: %s exports no ac5_mod_main; unloaded", m->id,
                    names[i]);
            FreeLibrary(h);
            m->natives_failed++;
            free(names[i]);
            continue;
        }
        ps2_log("mod: %s runs native code from %s", m->id, names[i]);
        prev = ps2_modapi_enter(api);
        rc = entry(api);
        ps2_modapi_leave(prev);
        if (rc) {
            ps2_log("mod: %s: %s's ac5_mod_main returned %d (failure); anything "
                    "it attached first stays attached", m->id, names[i], rc);
            m->natives_failed++;
        } else {
            m->natives_ok++;
        }
#else
        ps2_log("mod: %s: native modules are loaded on Windows only; %s skipped",
                m->id, names[i]);
        m->natives_failed++;
#endif
        free(names[i]);
    }
}

static void data_files_load(mod_info *m) {
    char path[700];
    DIR *d;
    struct dirent *e;
    char *names[64];
    unsigned n = 0;
    snprintf(path, sizeof path, "%s/symbols.txt", m->dir);
    m->symbols = symbols_load(path, m->id);
    snprintf(path, sizeof path, "%s/params.txt", m->dir);
    m->params = (u32)ps2_params_load(path, m->priority, m->id);
    d = opendir(m->dir);
    if (!d) return;
    while ((e = readdir(d)) != NULL && n < 64) {
        size_t len = strlen(e->d_name);
        if (len > 6 && (!strcmp(e->d_name + len - 6, ".pnach")
                        || !strcmp(e->d_name + len - 6, ".PNACH"))) {
            names[n] = strdup(e->d_name);
            if (names[n]) n++;
        }
    }
    closedir(d);
    if (n > 1) qsort(names, n, sizeof *names, str_cmp);
    for (unsigned i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s/%s", m->dir, names[i]);
        m->patches += (u32)ps2_patch_load_pnach(path, m->priority, m->id);
        free(names[i]);
    }
}

void ps2_mod_start(void) {
    if (mods_off) return;
    if (ps2_vfs_pac_changed()) archive_driver_install();
    for (u32 k = 0; k < ps2_mod_count(); k++) {
        mod_info *m = &mods[load_order[k]];
        if (m->enabled) data_files_load(m);
    }
    ps2_patch_start();
    ps2_params_start();
    for (u32 k = 0; k < ps2_mod_count(); k++) {
        mod_info *m = &mods[load_order[k]];
        if (m->enabled) natives_load(m);
    }
    if (nmods) ps2_lua_start();
}

void ps2_mod_report(void) {
    ps2_modapi_shutdown();
    if (nmods) {
        ps2_log("---- mods ----");
        for (u32 i = 0; i < nmods; i++) {
            const mod_info *m = &mods[i];
            u32 won;
            s64 heap;
            char native[64] = "";
            ps2_vfs_owner_stats(m->id, &won, &heap);
            if (m->natives_ok || m->natives_failed)
                snprintf(native, sizeof native, ", native %u ok / %u failed",
                         m->natives_ok, m->natives_failed);
            ps2_log("   %-24s %-10s priority %-4d %s%s%s%u file(s) filling %u "
                    "place(s), guest allocation %+lld%s",
                    m->name[0] ? m->name : m->id,
                    m->version[0] ? m->version : "-", m->priority,
                    m->author[0] ? "by " : "", m->author[0] ? m->author : "",
                    m->author[0] ? ", " : "", m->files, won, (long long)heap,
                    native);
            if (m->patches || m->params || m->symbols)
                ps2_log("      %u patch(es), %u parameter override(s), %u "
                        "name(s)", m->patches, m->params, m->symbols);
            if (!m->enabled)
                ps2_log("      switched off: %s", m->off_why);
        }
        if (archive_served || archive_passed)
            ps2_log("   DATA.PAC loads: %llu member(s) served from the "
                    "filesystem, %llu left to the game's decompressor",
                    (unsigned long long)archive_served,
                    (unsigned long long)archive_passed);
        ps2_modapi_report();
    }
    ps2_patch_report();
    ps2_params_report();
    ps2_vfs_report();
}
