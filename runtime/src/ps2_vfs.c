#ifndef _WIN32
#define _FILE_OFFSET_BITS 64
#endif
#include "ps2_runtime.h"
#include "ps2_hle.h"
#include "ps2_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pthread.h>

#define SECTOR        PS2_VFS_SECTOR
#define FIRST_LSN     32u
#define ULZ_MAGIC     0x1A7A6C55u
#define ULZ_HEADER    16u
#define MEMBER_ALIGN  16u
#define PAC_PREFIX    "BIN/DATA.PAC/"

typedef enum { SRC_IMAGE, SRC_TREE, SRC_HOST, SRC_MEMORY } vfs_src;

typedef struct vfs_claim vfs_claim;

typedef struct {
    ps2_disc_file f;
    vfs_src src;
    u64 image_off;
    char *host;
    u8 *mem;
    u32 disc_size;
    const vfs_claim *claim;
    int failed;
} vfs_node;

static vfs_node **nodes;
static u32 nnodes, cnodes;
static u32 *slots;
static u32 nslots;
static int vfs_is_tree, vfs_opened;
static char vfs_path[1024];
static FILE *image_fp;
static u32 end_lsn;
static pthread_mutex_t vfs_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 reads_sectors, reads_bytes, reads_from_mods;

typedef enum { CL_PENDING, CL_DISC, CL_NEW, CL_PAC, CL_BAD } claim_kind;

struct vfs_claim {
    char *path;
    char *host;
    u64 size;
    int priority;
    unsigned order;
    const char *owner;
    claim_kind kind;
    int scoped;
    u32 won;
    s64 heap_delta;
    u32 absent, absent_first;
    struct { u32 size, count, first; } sizes[4];
    u32 nsizes, more_sizes;
};

static vfs_claim **claims;
static u32 nclaims, cclaims;
static u32 nconflicts;

typedef struct { u32 index; vfs_claim *claim; } pac_over;

typedef struct {
    u32 off, packed, unpacked;
    u32 size;
    int loaded;
    u32 nfiles;
    u32 *offs;
    u32 nover;
    pac_over *over;
} pac_member;

typedef struct { char *name; u32 member, index; } pac_name;

static pac_member *pac;
static u32 pac_count, pac_nchanged;
static vfs_node *pac_node, *tbl_node;
static struct { vfs_src src; u8 *mem; char *host; u64 image_off; } tbl_orig;
static pac_name *pnames;
static u32 npnames;
static u32 *pnames_sorted;
static u64 pac_fills, pac_fill_bytes;

static u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static void wr32(u8 *p, u32 v) { memcpy(p, &v, 4); }
static u32 align16(u32 n) { return (n + MEMBER_ALIGN - 1u) & ~(MEMBER_ALIGN - 1u); }
static u32 sectors_of(u32 size) { return size ? (size + SECTOR - 1u) / SECTOR : 1u; }

static char upc(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static int ieq(const char *a, const char *b) {
    while (*a && *b && upc(*a) == upc(*b)) { a++; b++; }
    return !*a && !*b;
}

static int icmp(const char *a, const char *b) {
    while (*a && *b && upc(*a) == upc(*b)) { a++; b++; }
    return (unsigned char)upc(*a) - (unsigned char)upc(*b);
}

static int iprefix(const char *s, const char *prefix) {
    while (*prefix && upc(*s) == upc(*prefix)) { s++; prefix++; }
    return !*prefix;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (!d) ps2_fatal("vfs: out of memory");
    memcpy(d, s, n);
    return d;
}

static int norm_path(const char *name, char *out, size_t cap) {
    const char *p = name;
    size_t n = 0;
    for (const char *q = p; *q && (q - p) < 16; q++) {
        if (*q == '/' || *q == '\\') break;
        if (*q == ':') { p = q + 1; break; }
    }
    for (; *p; p++) {
        char c = *p == '\\' ? '/' : *p;
        if (c == ';') break;
        if (c == '/' && (n == 0 || out[n - 1] == '/')) continue;
        if (n + 1 >= cap) return -1;
        out[n++] = c;
    }
    while (n && out[n - 1] == '/') n--;
    out[n] = 0;
    return (int)n;
}

#ifdef _WIN32
static int seek64(FILE *fp, u64 off) { return _fseeki64(fp, (long long)off, SEEK_SET); }
#else
static int seek64(FILE *fp, u64 off) { return fseeko(fp, (off_t)off, SEEK_SET); }
#endif

static int host_stat(const char *path, u64 *size, int *is_dir) {
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return -1;
#else
    struct stat st;
    if (stat(path, &st) != 0) return -1;
#endif
    if (size) *size = (u64)st.st_size;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode);
    return 0;
}

#define VFS_HANDLES 8
static struct { const char *path; FILE *fp; u64 tick; } handles[VFS_HANDLES];
static u64 handle_tick;

static FILE *handle_for(const char *path) {
    unsigned i, victim = 0;
    for (i = 0; i < VFS_HANDLES; i++) {
        if (handles[i].fp && handles[i].path == path) {
            handles[i].tick = ++handle_tick;
            return handles[i].fp;
        }
    }
    for (i = 0; i < VFS_HANDLES; i++) {
        if (!handles[i].fp) { victim = i; break; }
        if (handles[i].tick < handles[victim].tick) victim = i;
    }
    if (handles[victim].fp) fclose(handles[victim].fp);
    handles[victim].fp = fopen(path, "rb");
    handles[victim].path = handles[victim].fp ? path : NULL;
    handles[victim].tick = ++handle_tick;
    return handles[victim].fp;
}

static void guest_put(u32 addr, const u8 *src, u32 n) {
    while (n) {
        u32 off = addr & PS2_PAGE_MASK;
        u32 chunk = PS2_PAGE_SIZE - off;
        u8 *p = ps2_pt[addr >> PS2_PAGE_BITS];
        if (chunk > n) chunk = n;
        if (p) memcpy(p + off, src, chunk);
        else for (u32 i = 0; i < chunk; i++) ps2_w8(addr + i, src[i]);
        addr += chunk;
        src += chunk;
        n -= chunk;
    }
}

static u32 hash_name(const char *s) {
    u32 h = 2166136261u;
    for (; *s; s++) { h ^= (u8)upc(*s); h *= 16777619u; }
    return h;
}

static void hash_insert(u32 index) {
    u32 i = hash_name(nodes[index]->f.name) & (nslots - 1u);
    while (slots[i]) i = (i + 1u) & (nslots - 1u);
    slots[i] = index + 1u;
}

static void hash_grow(void) {
    u32 want = 1024;
    while (want < nnodes * 2u + 2u) want *= 2u;
    if (want <= nslots) return;
    free(slots);
    slots = (u32 *)calloc(want, sizeof *slots);
    if (!slots) ps2_fatal("vfs: out of memory indexing the disc");
    nslots = want;
    for (u32 i = 0; i < nnodes; i++) hash_insert(i);
}

static vfs_node *lookup(const char *normalised) {
    u32 i;
    if (!nslots) return NULL;
    for (i = hash_name(normalised) & (nslots - 1u); slots[i];
         i = (i + 1u) & (nslots - 1u)) {
        vfs_node *n = nodes[slots[i] - 1u];
        if (ieq(n->f.name, normalised)) return n;
    }
    return NULL;
}

static vfs_node *add_node(const char *name, u32 size) {
    vfs_node *n;
    if (strlen(name) >= sizeof n->f.name) {
        ps2_log("vfs: path too long for the disc index, skipped: %s", name);
        return NULL;
    }
    if (nnodes == cnodes) {
        cnodes = cnodes ? cnodes * 2u : 256u;
        nodes = (vfs_node **)realloc(nodes, cnodes * sizeof *nodes);
        if (!nodes) ps2_fatal("vfs: out of memory building the disc index");
    }
    n = (vfs_node *)calloc(1, sizeof *n);
    if (!n) ps2_fatal("vfs: out of memory building the disc index");
    snprintf(n->f.name, sizeof n->f.name, "%s", name);
    n->f.size = size;
    n->disc_size = size;
    nodes[nnodes++] = n;
    if (nnodes * 2u >= nslots) hash_grow();
    else hash_insert(nnodes - 1u);
    return n;
}

static void node_read(vfs_node *n, u64 pos, u32 len, u8 *dst) {
    u32 data = 0;
    if (pos < n->f.size) {
        u64 left = (u64)n->f.size - pos;
        data = left < len ? (u32)left : len;
    }
    if (data < len) memset(dst + data, 0, len - data);
    if (!data) return;
    switch (n->src) {
    case SRC_MEMORY:
        memcpy(dst, n->mem + pos, data);
        return;
    case SRC_IMAGE:
        if (image_fp && seek64(image_fp, n->image_off + pos) == 0
            && fread(dst, 1, data, image_fp) == data)
            return;
        break;
    case SRC_TREE:
    case SRC_HOST: {
        FILE *fp = handle_for(n->host);
        size_t got;
        if (!fp || seek64(fp, pos) != 0) break;
        got = fread(dst, 1, data, fp);
        if (n->src == SRC_HOST) reads_from_mods++;
        if (got == data) return;
        memset(dst + got, 0, data - got);
        if (!n->failed) {
            n->failed = 1;
            ps2_log("vfs: '%s' is shorter on disk than when it was indexed "
                    "(%s); the rest reads as zeros", n->f.name, n->host);
        }
        return;
    }
    }
    memset(dst, 0, data);
    if (!n->failed) {
        n->failed = 1;
        ps2_log("vfs: cannot read '%s' from %s; it reads as zeros", n->f.name,
                n->src == SRC_IMAGE ? "the disc image" : n->host);
    }
}

static vfs_node *node_at(u32 lsn) {
    u32 lo = 0, hi = nnodes;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        vfs_node *n = nodes[mid];
        if (lsn < n->f.lsn) hi = mid;
        else if (lsn >= n->f.lsn + sectors_of(n->f.size)) lo = mid + 1u;
        else return n;
    }
    return NULL;
}

int ps2_vfs_read_sectors(u32 lsn, u32 sectors, void *dst) {
    u8 *out = (u8 *)dst;
    u32 done = 0;
    if (!vfs_opened) return -1;
    pthread_mutex_lock(&vfs_lock);
    reads_sectors += sectors;
    while (done < sectors) {
        vfs_node *n = node_at(lsn + done);
        u32 in, run;
        if (!n) {
            memset(out + (size_t)done * SECTOR, 0, SECTOR);
            done++;
            continue;
        }
        in = lsn + done - n->f.lsn;
        run = sectors_of(n->f.size) - in;
        if (run > sectors - done) run = sectors - done;
        node_read(n, (u64)in * SECTOR, run * SECTOR, out + (size_t)done * SECTOR);
        done += run;
    }
    pthread_mutex_unlock(&vfs_lock);
    return (int)sectors;
}

int ps2_vfs_read_sectors_guest(u32 lsn, u32 sectors, u32 guest_addr) {
    enum { CHUNK = 256 };
    u8 *buf;
    u32 done = 0;
    if (!vfs_opened) return -1;
    buf = (u8 *)malloc((size_t)(sectors < CHUNK ? (sectors ? sectors : 1u) : CHUNK)
                       * SECTOR);
    if (!buf) return -1;
    while (done < sectors) {
        u32 run = sectors - done < CHUNK ? sectors - done : CHUNK;
        ps2_vfs_read_sectors(lsn + done, run, buf);
        guest_put(guest_addr + done * SECTOR, buf, run * SECTOR);
        done += run;
    }
    free(buf);
    return (int)sectors;
}

int ps2_vfs_read(const ps2_disc_file *f, u64 pos, u32 len, void *dst) {
    vfs_node *n = (vfs_node *)(void *)f;
    if (!f || !vfs_opened) return -1;
    if (pos >= f->size) return 0;
    if (pos + len > f->size) len = (u32)((u64)f->size - pos);
    pthread_mutex_lock(&vfs_lock);
    node_read(n, pos, len, (u8 *)dst);
    reads_bytes += len;
    pthread_mutex_unlock(&vfs_lock);
    return (int)len;
}

int ps2_vfs_read_guest(const ps2_disc_file *f, u64 pos, u32 len,
                       u32 guest_addr) {
    enum { CHUNK = 1u << 20 };
    u8 *buf;
    u32 done = 0;
    if (!f || !vfs_opened) return -1;
    if (pos >= f->size) return 0;
    if (pos + len > f->size) len = (u32)((u64)f->size - pos);
    buf = (u8 *)malloc(len < CHUNK ? (len ? len : 1u) : CHUNK);
    if (!buf) return -1;
    while (done < len) {
        u32 run = len - done < CHUNK ? len - done : CHUNK;
        ps2_vfs_read(f, pos + done, run, buf);
        guest_put(guest_addr + done, buf, run);
        done += run;
    }
    free(buf);
    return (int)len;
}

const ps2_disc_file *ps2_vfs_find(const char *name) {
    char norm[256];
    vfs_node *n;
    if (!name || norm_path(name, norm, sizeof norm) <= 0) return NULL;
    n = lookup(norm);
    return n ? &n->f : NULL;
}

u32 ps2_vfs_file_count(void) { return nnodes; }
int ps2_vfs_ready(void) { return vfs_opened; }

static int image_sector(u32 lsn, u8 *dst) {
    return (image_fp && seek64(image_fp, (u64)lsn * SECTOR) == 0
            && fread(dst, 1, SECTOR, image_fp) == SECTOR) ? 0 : -1;
}

static void image_scan_dir(u32 lsn, u32 size, const char *prefix, int depth) {
    u8 buf[SECTOR];
    u32 sectors = (size + SECTOR - 1u) / SECTOR;
    for (u32 s = 0; s < sectors; s++) {
        u32 off = 0;
        if (image_sector(lsn + s, buf) != 0) break;
        while (off < SECTOR) {
            u8 len = buf[off];
            u32 elsn, esize, nlen;
            char name[128], full[256];
            if (len == 0 || off + len > SECTOR) break;
            memcpy(&elsn, buf + off + 2, 4);
            memcpy(&esize, buf + off + 10, 4);
            nlen = buf[off + 32];
            if (nlen == 1 && (buf[off + 33] == 0 || buf[off + 33] == 1)) {
                off += len;
                continue;
            }
            if (nlen > sizeof name - 1) nlen = sizeof name - 1;
            memcpy(name, buf + off + 33, nlen);
            name[nlen] = 0;
            for (u32 i = 0; i < nlen; i++)
                if (name[i] == ';') { name[i] = 0; break; }
            snprintf(full, sizeof full, "%s%s%s", prefix, *prefix ? "/" : "",
                     name);
            if (buf[off + 25] & 2) {
                if (depth < 8) image_scan_dir(elsn, esize, full, depth + 1);
            } else {
                vfs_node *n = add_node(full, esize);
                if (n) {
                    n->src = SRC_IMAGE;
                    n->image_off = (u64)elsn * SECTOR;
                    memcpy(n->f.date, buf + off + 18, 7);
                }
            }
            off += len;
        }
    }
}

static void tree_scan(const char *hostdir, const char *prefix, int depth) {
    DIR *d = opendir(hostdir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        char hp[1024], full[256];
        u64 size;
        int is_dir, hn, fn;
        if (e->d_name[0] == '.') continue;
        hn = snprintf(hp, sizeof hp, "%s/%s", hostdir, e->d_name);
        fn = snprintf(full, sizeof full, "%s%s%s", prefix, *prefix ? "/" : "",
                      e->d_name);
        if (hn < 0 || (size_t)hn >= sizeof hp || fn < 0
            || (size_t)fn >= sizeof full) {
            ps2_log("vfs: path too long, not indexed: %s/%s", hostdir, e->d_name);
            continue;
        }
        if (host_stat(hp, &size, &is_dir) != 0) continue;
        if (is_dir) {
            if (depth < 8) tree_scan(hp, full, depth + 1);
        } else if (size > 0xFFFFFFFFull) {
            ps2_log("vfs: '%s' is larger than a disc file can be; not indexed",
                    hp);
        } else {
            vfs_node *n = add_node(full, (u32)size);
            if (n) {
                n->src = SRC_TREE;
                n->host = xstrdup(hp);
            }
        }
    }
    closedir(d);
}

typedef struct {
    const u8 *ctrl, *lit, *tok;
    u32 nctrl, nlit, ntok, shift;
} ulz_streams;

static s64 ulz_run(const ulz_streams *s, u8 *dst, u32 want) {
    u32 n = 0, c = 0, l = 0, t = 0;
    u32 mask = (1u << s->shift) - 1u;
    while (n < want) {
        u32 cw;
        if (c + 4u > s->nctrl) return -1;
        cw = rd32(s->ctrl + c);
        c += 4u;
        for (int b = 0; b < 32 && n < want; b++, cw <<= 1) {
            if (cw & 0x80000000u) {
                if (l >= s->nlit) return -1;
                dst[n++] = s->lit[l++];
            } else {
                u32 tokv, dist, run;
                if (t + 2u > s->ntok) return -1;
                tokv = (u32)s->tok[t] | ((u32)s->tok[t + 1] << 8);
                t += 2u;
                dist = (tokv & mask) + 1u;
                run = (tokv >> s->shift) + 3u;
                if (dist > n) return -1;
                for (u32 i = 0; i < run && n < want; i++, n++)
                    dst[n] = dst[n - dist];
            }
        }
    }
    return n;
}

static int ulz_header(const u8 *h, u32 len, u32 *size, u32 *shift, u32 *lit,
                      u32 *tok) {
    if (len < ULZ_HEADER || rd32(h) != ULZ_MAGIC) return -1;
    *size = rd32(h + 4) & 0xFFFFFFu;
    *shift = rd32(h + 8) >> 24;
    *lit = rd32(h + 8) & 0xFFFFFFu;
    *tok = rd32(h + 12) & 0xFFFFFFu;
    return (*shift >= 1u && *shift <= 15u) ? 0 : -1;
}

s64 ps2_vfs_ulz_decode(const u8 *src, u32 len, u8 *dst, u32 cap) {
    u32 size, shift, lit, tok;
    ulz_streams s;
    if (ulz_header(src, len, &size, &shift, &lit, &tok) != 0) return -1;
    if (lit > len || tok > len) return -1;
    s.ctrl = src + ULZ_HEADER; s.nctrl = len - ULZ_HEADER;
    s.lit = src + lit;         s.nlit = len - lit;
    s.tok = src + tok;         s.ntok = len - tok;
    s.shift = shift;
    return ulz_run(&s, dst, size < cap ? size : cap);
}

static void pac_read(u64 pos, u32 len, u8 *dst) {
    u64 end = (u64)pac_node->f.size;
    u32 have = pos < end ? (u32)((end - pos) < len ? end - pos : len) : 0;
    if (have) node_read(pac_node, pos, have, dst);
    if (have < len) memset(dst + have, 0, len - have);
}

static s64 pac_prefix(u32 m, u8 *dst, u32 want) {
    const pac_member *pm = &pac[m];
    u8 head[ULZ_HEADER];
    u32 size, shift, lit, tok, wc, wl, wt;
    u8 *bc, *bl, *bt;
    ulz_streams s;
    s64 got;
    if (pm->packed < ULZ_HEADER) return -1;
    pac_read(pm->off, ULZ_HEADER, head);
    if (ulz_header(head, pm->packed, &size, &shift, &lit, &tok) != 0) return -1;
    if (lit > pm->packed || tok > pm->packed) return -1;
    if (want > size) want = size;
    wc = (want / 32u + 2u) * 4u;
    wl = want + 4u;
    wt = (want / 3u + 2u) * 2u;
    if (wc > pm->packed - ULZ_HEADER) wc = pm->packed - ULZ_HEADER;
    if (wl > pm->packed - lit) wl = pm->packed - lit;
    if (wt > pm->packed - tok) wt = pm->packed - tok;
    bc = (u8 *)malloc(wc + 1u);
    bl = (u8 *)malloc(wl + 1u);
    bt = (u8 *)malloc(wt + 1u);
    if (!bc || !bl || !bt) ps2_fatal("vfs: out of memory reading an archive member");
    pac_read((u64)pm->off + ULZ_HEADER, wc, bc);
    pac_read((u64)pm->off + lit, wl, bl);
    pac_read((u64)pm->off + tok, wt, bt);
    s.ctrl = bc; s.nctrl = wc;
    s.lit = bl;  s.nlit = wl;
    s.tok = bt;  s.ntok = wt;
    s.shift = shift;
    got = ulz_run(&s, dst, want);
    free(bc); free(bl); free(bt);
    return got;
}

static int pac_load_header(u32 m) {
    pac_member *pm = &pac[m];
    u8 small[4096];
    u32 n, need;
    s64 got;
    if (pm->loaded) return 0;
    got = pac_prefix(m, small, sizeof small);
    if (got < 4) return -1;
    n = rd32(small);
    if (n > 0x10000u || 4u + 4u * n > pm->unpacked) return -1;
    need = 4u + 4u * n;
    pm->offs = (u32 *)malloc((n ? n : 1u) * sizeof *pm->offs);
    if (!pm->offs) ps2_fatal("vfs: out of memory reading an archive member");
    if ((u64)got >= need) {
        for (u32 k = 0; k < n; k++) pm->offs[k] = rd32(small + 4u + 4u * k);
    } else {
        u8 *big = (u8 *)malloc(need);
        if (!big) ps2_fatal("vfs: out of memory reading an archive member");
        if (pac_prefix(m, big, need) < (s64)need) {
            free(big);
            free(pm->offs);
            pm->offs = NULL;
            return -1;
        }
        for (u32 k = 0; k < n; k++) pm->offs[k] = rd32(big + 4u + 4u * k);
        free(big);
    }
    for (u32 k = 0; k < n; k++) {
        if (pm->offs[k] >= pm->unpacked || (pm->offs[k] && pm->offs[k] < need)) {
            free(pm->offs);
            pm->offs = NULL;
            return -1;
        }
    }
    pm->nfiles = n;
    pm->loaded = 1;
    return 0;
}

static u32 pac_extent(const pac_member *pm, u32 k) {
    u32 start = pm->offs[k], next = pm->unpacked;
    if (!start) return 0;
    for (u32 j = 0; j < pm->nfiles; j++)
        if (pm->offs[j] > start && pm->offs[j] < next) next = pm->offs[j];
    return next - start;
}

static const pac_over *pac_override(const pac_member *pm, u32 k) {
    u32 lo = 0, hi = pm->nover;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2u;
        if (pm->over[mid].index == k) return &pm->over[mid];
        if (pm->over[mid].index < k) lo = mid + 1u;
        else hi = mid;
    }
    return NULL;
}

static u64 pac_layout(const pac_member *pm, u32 *new_offs) {
    u64 at = align16(4u + 4u * pm->nfiles);
    for (u32 k = 0; k < pm->nfiles; k++) {
        const pac_over *ov = pac_override(pm, k);
        u64 len;
        if (!ov && !pm->offs[k]) {
            if (new_offs) new_offs[k] = 0;
            continue;
        }
        len = ov ? ov->claim->size : pac_extent(pm, k);
        if (at > 0xFFFFFFFFull) return at;
        if (new_offs) new_offs[k] = (u32)at;
        at = (at + len + MEMBER_ALIGN - 1u) & ~(u64)(MEMBER_ALIGN - 1u);
    }
    return at;
}

static int pname_cmp_pos(const void *a, const void *b) {
    const pac_name *x = (const pac_name *)a, *y = (const pac_name *)b;
    if (x->member != y->member) return x->member < y->member ? -1 : 1;
    return x->index < y->index ? -1 : (x->index > y->index);
}

static int pname_cmp_name(const void *a, const void *b) {
    const pac_name *x = &pnames[*(const u32 *)a], *y = &pnames[*(const u32 *)b];
    int c = icmp(x->name, y->name);
    if (c) return c;
    if (x->member != y->member) return x->member < y->member ? -1 : 1;
    return x->index < y->index ? -1 : (x->index > y->index);
}

static int names_loaded;

static void names_load(void) {
    static const char *where[] = { NULL, "config/pac_names.txt",
                                   "pac_names.txt", "../config/pac_names.txt" };
    char line[512];
    FILE *fp = NULL;
    u32 cap = 0;
    if (names_loaded || !pac) return;
    names_loaded = 1;
    where[0] = getenv("PS2_MOD_NAMES");
    for (unsigned i = 0; i < sizeof where / sizeof where[0]; i++) {
        if (!where[i] || !*where[i]) continue;
        fp = fopen(where[i], "r");
        if (fp) { ps2_log("vfs: archive names from %s", where[i]); break; }
    }
    if (!fp) {
        ps2_log("vfs: no pac_names.txt, so files inside DATA.PAC can only be "
                "named by position (BIN/DATA.PAC/<member>/#<index>).  "
                "Generate it with: python -m modkit.export_names --disc DIR "
                "--names datapack.bin -o config/pac_names.txt");
        return;
    }
    while (fgets(line, sizeof line, fp)) {
        unsigned long m, k;
        char *p = line, *end, *name;
        size_t len;
        if (*p == '#') continue;
        m = strtoul(p, &end, 10);
        if (end == p) continue;
        p = end;
        k = strtoul(p, &end, 10);
        if (end == p) continue;
        name = end;
        while (*name == ' ' || *name == '\t') name++;
        len = strlen(name);
        while (len && (name[len - 1] == '\n' || name[len - 1] == '\r'
                       || name[len - 1] == ' ' || name[len - 1] == '\t'))
            name[--len] = 0;
        if (!len || m >= pac_count) continue;
        if (npnames == cap) {
            pac_name *grown;
            cap = cap ? cap * 2u : 16384u;
            grown = (pac_name *)realloc(pnames, cap * sizeof *pnames);
            if (!grown) ps2_fatal("vfs: out of memory loading archive names");
            pnames = grown;
        }
        pnames[npnames].name = xstrdup(name);
        pnames[npnames].member = (u32)m;
        pnames[npnames].index = (u32)k;
        npnames++;
    }
    fclose(fp);
    qsort(pnames, npnames, sizeof *pnames, pname_cmp_pos);
    pnames_sorted = (u32 *)malloc((npnames ? npnames : 1u) * sizeof *pnames_sorted);
    if (!pnames_sorted) ps2_fatal("vfs: out of memory loading archive names");
    for (u32 i = 0; i < npnames; i++) pnames_sorted[i] = i;
    qsort(pnames_sorted, npnames, sizeof *pnames_sorted, pname_cmp_name);
    ps2_log("vfs: %u archive names", npnames);
}

static u32 names_find(const char *name, u32 *first) {
    u32 lo = 0, hi, start, end;
    names_load();
    hi = npnames;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2u;
        if (icmp(pnames[pnames_sorted[mid]].name, name) < 0) lo = mid + 1u;
        else hi = mid;
    }
    start = end = lo;
    while (end < npnames && !icmp(pnames[pnames_sorted[end]].name, name)) end++;
    *first = start;
    return end - start;
}

static const char *name_of(u32 member, u32 index) {
    u32 lo = 0, hi;
    names_load();
    hi = npnames;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2u;
        const pac_name *p = &pnames[mid];
        if (p->member == member && p->index == index) return p->name;
        if (p->member < member || (p->member == member && p->index < index))
            lo = mid + 1u;
        else hi = mid;
    }
    return NULL;
}

static int parse_scoped(const char *path, u32 *member, u32 *index) {
    const char *p = path + strlen(PAC_PREFIX), *rest;
    char *end;
    unsigned long m = strtoul(p, &end, 10);
    if (end == p || *end != '/' || m >= pac_count) return -1;
    rest = end + 1;
    if (*rest == '#') {
        unsigned long k = strtoul(rest + 1, &end, 10);
        if (end == rest + 1 || *end) return -1;
        if (pac_load_header((u32)m) != 0 || k >= pac[m].nfiles) return -1;
        *member = (u32)m;
        *index = (u32)k;
        return 0;
    } else {
        u32 first, count = names_find(rest, &first);
        for (u32 i = 0; i < count; i++) {
            const pac_name *pn = &pnames[pnames_sorted[first + i]];
            if (pn->member == m) { *member = pn->member; *index = pn->index; return 0; }
        }
    }
    return -1;
}

static int pac_open(void) {
    u8 *tbl;
    u32 len;
    tbl_node = lookup("BIN/DATA.TBL");
    pac_node = lookup("BIN/DATA.PAC");
    if (!tbl_node || !pac_node) return -1;
    len = tbl_node->f.size;
    tbl = (u8 *)malloc(len ? len : 1u);
    if (!tbl) ps2_fatal("vfs: out of memory reading DATA.TBL");
    node_read(tbl_node, 0, len, tbl);
    if (len < 8 || (u64)8u + 12ull * rd32(tbl) != len) {
        ps2_log("vfs: DATA.TBL is %u bytes, which is not a table; the archive "
                "is not opened", len);
        free(tbl);
        tbl_node = pac_node = NULL;
        return -1;
    }
    pac_count = rd32(tbl);
    pac = (pac_member *)calloc(pac_count, sizeof *pac);
    if (!pac) ps2_fatal("vfs: out of memory reading DATA.TBL");
    for (u32 i = 0; i < pac_count; i++) {
        pac[i].off = rd32(tbl + 8u + 8u * i);
        pac[i].packed = rd32(tbl + 12u + 8u * i);
        pac[i].unpacked = rd32(tbl + 8u + 8u * pac_count + 4u * i);
        pac[i].size = pac[i].unpacked;
    }
    free(tbl);
    return 0;
}

static int claim_beats(const vfs_claim *a, const vfs_claim *b) {
    if (a->priority != b->priority) return a->priority > b->priority ? 1 : -1;
    if (a->scoped != b->scoped) return a->scoped ? 1 : -1;
    return a->order > b->order ? 1 : -1;
}

static void say_conflict(const char *what, const vfs_claim *win,
                         const vfs_claim *lose) {
    nconflicts++;
    ps2_log("vfs: conflict on %s -- %s (priority %d, '%s') wins over %s "
            "(priority %d, '%s')", what, win->owner, win->priority, win->path,
            lose->owner, lose->priority, lose->path);
}

typedef struct { u32 member, index; vfs_claim *claim; } pac_candidate;

static int cand_cmp(const void *a, const void *b) {
    const pac_candidate *x = (const pac_candidate *)a, *y = (const pac_candidate *)b;
    if (x->member != y->member) return x->member < y->member ? -1 : 1;
    if (x->index != y->index) return x->index < y->index ? -1 : 1;
    return claim_beats(x->claim, y->claim) > 0 ? 1 : -1;
}

typedef struct { vfs_claim *win, *lose; u32 files, member, index; } pac_clash;
static pac_clash *clashes;
static u32 nclashes, cclashes;

static void note_clash(vfs_claim *win, vfs_claim *lose, u32 member, u32 index) {
    for (u32 i = 0; i < nclashes; i++)
        if (clashes[i].win == win && clashes[i].lose == lose) {
            clashes[i].files++;
            return;
        }
    if (nclashes == cclashes) {
        cclashes = cclashes ? cclashes * 2u : 16u;
        clashes = (pac_clash *)realloc(clashes, cclashes * sizeof *clashes);
        if (!clashes) ps2_fatal("vfs: out of memory resolving mods");
    }
    clashes[nclashes].win = win;
    clashes[nclashes].lose = lose;
    clashes[nclashes].files = 1;
    clashes[nclashes].member = member;
    clashes[nclashes].index = index;
    nclashes++;
}

static void resolve_archive(vfs_claim **pending, u32 npending) {
    pac_candidate *cand = NULL;
    u32 ncand = 0, ccand = 0;
    u8 *tbl;

    for (u32 i = 0; i < npending; i++) {
        vfs_claim *c = pending[i];
        u32 member, index, first, count;
        if (c->scoped) {
            if (parse_scoped(c->path, &member, &index) != 0) {
                c->kind = CL_BAD;
                ps2_log("vfs: %s's '%s' does not name a file in DATA.PAC -- "
                        "use BIN/DATA.PAC/<member>/<name> with a name from "
                        "pac_names.txt, or /#<index>", c->owner, c->path);
                continue;
            }
            first = 0;
            count = 1;
        } else {
            count = names_find(c->path, &first);
        }
        for (u32 k = 0; k < count; k++) {
            if (ncand == ccand) {
                ccand = ccand ? ccand * 2u : 256u;
                cand = (pac_candidate *)realloc(cand, ccand * sizeof *cand);
                if (!cand) ps2_fatal("vfs: out of memory resolving mods");
            }
            if (!c->scoped) {
                member = pnames[pnames_sorted[first + k]].member;
                index = pnames[pnames_sorted[first + k]].index;
            }
            cand[ncand].member = member;
            cand[ncand].index = index;
            cand[ncand].claim = c;
            ncand++;
        }
        c->kind = CL_PAC;
    }
    if (!ncand) { free(cand); return; }

    {
        u32 kept = 0;
        for (u32 i = 0; i < ncand; i++) {
            pac_candidate *c = &cand[i];
            vfs_claim *cl = c->claim;
            pac_member *pm = &pac[c->member];
            if (!cl->scoped && pac_load_header(c->member) == 0
                && c->index < pm->nfiles) {
                u32 ext = pac_extent(pm, c->index), s;
                if (!pm->offs[c->index]) {
                    if (!cl->absent++) cl->absent_first = c->member;
                    continue;
                }
                for (s = 0; s < cl->nsizes && cl->sizes[s].size != ext; s++) {}
                if (s < cl->nsizes) cl->sizes[s].count++;
                else if (cl->nsizes < 4) {
                    cl->sizes[s].size = ext;
                    cl->sizes[s].count = 1;
                    cl->sizes[s].first = c->member;
                    cl->nsizes++;
                } else cl->more_sizes++;
            }
            cand[kept++] = *c;
        }
        ncand = kept;
        for (u32 i = 0; i < npending; i++) {
            vfs_claim *cl = pending[i];
            if (cl->absent)
                ps2_log("vfs: %s's '%s' is not applied to %u place(s) the name "
                        "table lists but the disc leaves empty (first: member "
                        "%u); a plain name only replaces copies that exist -- "
                        "BIN/DATA.PAC/%04u/%s adds one", cl->owner, cl->path,
                        cl->absent, cl->absent_first, cl->absent_first, cl->path);
            if (cl->nsizes > 1) {
                char sizes[256];
                int at = 0;
                for (u32 s = 0; s < cl->nsizes && at < (int)sizeof sizes - 48; s++)
                    at += snprintf(sizes + at, sizeof sizes - (size_t)at,
                                   "%s%u bytes x%u (first member %u)", s ? ", " : "",
                                   cl->sizes[s].size, cl->sizes[s].count,
                                   cl->sizes[s].first);
                ps2_log("vfs: WARNING: the copies of '%s' that %s replaces are "
                        "not all the same size on the disc: %s%s.  The name "
                        "table may not mean the same file in all of them; "
                        "check the odd ones out, and name members explicitly "
                        "(BIN/DATA.PAC/<member>/%s) if they are not what you "
                        "meant to replace", cl->path, cl->owner, sizes,
                        cl->more_sizes ? ", and more" : "", cl->path);
            }
        }
        if (!ncand) { free(cand); return; }
    }
    qsort(cand, ncand, sizeof *cand, cand_cmp);

    for (u32 i = 0; i < ncand;) {
        u32 j = i, m = cand[i].member;
        pac_member *pm = &pac[m];
        while (j < ncand && cand[j].member == m) j++;
        if (pac_load_header(m) != 0) {
            ps2_log("vfs: DATA.PAC member %u cannot be read; %u mod file(s) "
                    "for it are not applied", m, j - i);
            i = j;
            continue;
        }
        pm->over = (pac_over *)calloc(j - i, sizeof *pm->over);
        if (!pm->over) ps2_fatal("vfs: out of memory resolving mods");
        for (u32 a = i; a < j;) {
            u32 b = a;
            vfs_claim *win;
            while (b < j && cand[b].index == cand[a].index) b++;
            win = cand[b - 1].claim;
            if (cand[a].index >= pm->nfiles) {
                ps2_log("vfs: pac_names.txt puts a file at member %u index %u, "
                        "but that member holds %u files; %s's '%s' is not "
                        "applied there -- regenerate pac_names.txt", m,
                        cand[a].index, pm->nfiles, win->owner, win->path);
                a = b;
                continue;
            }
            for (u32 x = a; x + 1u < b; x++)
                note_clash(win, cand[x].claim, m, cand[a].index);
            pm->over[pm->nover].index = cand[a].index;
            pm->over[pm->nover].claim = win;
            pm->nover++;
            a = b;
        }
        {
            u64 size = pac_layout(pm, NULL);
            if (size > PS2_RAM_SIZE) {
                ps2_log("vfs: DATA.PAC member %u would be %llu bytes with its "
                        "mods, more than the machine's whole 32 MB; its mod "
                        "files are not applied", m, (unsigned long long)size);
                free(pm->over);
                pm->over = NULL;
                pm->nover = 0;
            } else {
                for (u32 o = 0; o < pm->nover; o++) {
                    pac_over *ov = &pm->over[o];
                    u32 was = pac_extent(pm, ov->index);
                    ov->claim->won++;
                    ov->claim->heap_delta += (s64)align16((u32)ov->claim->size) - (s64)was;
                }
                pm->size = (u32)size;
                pac_nchanged++;
            }
        }
        i = j;
    }
    free(cand);
    for (u32 c = 0; c < nclashes; c++) {
        const pac_clash *k = &clashes[c];
        const char *nm = name_of(k->member, k->index);
        nconflicts += k->files;
        ps2_log("vfs: conflict on %u archive file(s), e.g. member %u file %u "
                "(%s) -- %s (priority %d, '%s') wins over %s (priority %d, "
                "'%s')", k->files, k->member, k->index, nm ? nm : "unnamed",
                k->win->owner, k->win->priority, k->win->path, k->lose->owner,
                k->lose->priority, k->lose->path);
    }
    if (!pac_nchanged) return;

    tbl = (u8 *)malloc(tbl_node->f.size);
    if (!tbl) ps2_fatal("vfs: out of memory generating DATA.TBL");
    node_read(tbl_node, 0, tbl_node->f.size, tbl);
    for (u32 m = 0; m < pac_count; m++)
        wr32(tbl + 8u + 8u * pac_count + 4u * m, pac[m].size);
    tbl_orig.src = tbl_node->src;
    tbl_orig.mem = tbl_node->mem;
    tbl_orig.host = tbl_node->host;
    tbl_orig.image_off = tbl_node->image_off;
    tbl_node->src = SRC_MEMORY;
    tbl_node->mem = tbl;
    ps2_log("vfs: %u DATA.PAC member(s) changed by mods; DATA.TBL is generated",
            pac_nchanged);
}

void ps2_vfs_pac_revert(const char *why) {
    if (!pac_nchanged) return;
    ps2_log("vfs: archive mods are OFF -- %s", why ? why : "no reason given");
    free(tbl_node->mem);
    tbl_node->src = tbl_orig.src;
    tbl_node->mem = tbl_orig.mem;
    tbl_node->host = tbl_orig.host;
    tbl_node->image_off = tbl_orig.image_off;
    for (u32 m = 0; m < pac_count; m++) {
        for (u32 o = 0; o < pac[m].nover; o++) {
            pac[m].over[o].claim->won = 0;
            pac[m].over[o].claim->heap_delta = 0;
        }
        free(pac[m].over);
        pac[m].over = NULL;
        pac[m].nover = 0;
        pac[m].size = pac[m].unpacked;
    }
    pac_nchanged = 0;
}

u32 ps2_vfs_pac_changed(void) { return pac_nchanged; }

int ps2_vfs_pac_is_changed(u32 member) {
    return member < pac_count && pac[member].nover > 0;
}

u32 ps2_vfs_pac_size(u32 member) {
    return member < pac_count ? pac[member].size : 0;
}

static void claim_read(const vfs_claim *c, u8 *dst) {
    FILE *fp = handle_for(c->host);
    size_t got = 0;
    if (fp && seek64(fp, 0) == 0) got = fread(dst, 1, (size_t)c->size, fp);
    if (got < c->size) {
        memset(dst + got, 0, (size_t)(c->size - got));
        ps2_log("vfs: %s's '%s' gave %llu of %llu bytes (%s); the rest is "
                "zeros", c->owner, c->path, (unsigned long long)got,
                (unsigned long long)c->size, c->host);
    }
    reads_from_mods++;
}

static int compose_locked(u32 m, u8 **out, u32 *len) {
    pac_member *pm;
    u8 *packed, *orig, *buf;
    u32 *new_offs;
    u64 size;
    s64 got;
    if (m >= pac_count || !pac_node) return -1;
    pm = &pac[m];
    if (pac_load_header(m) != 0) return -1;
    packed = (u8 *)malloc(pm->packed ? pm->packed : 1u);
    orig = (u8 *)malloc(pm->unpacked ? pm->unpacked : 1u);
    new_offs = (u32 *)malloc((pm->nfiles ? pm->nfiles : 1u) * sizeof *new_offs);
    if (!packed || !orig || !new_offs)
        ps2_fatal("vfs: out of memory composing an archive member");
    pac_read(pm->off, pm->packed, packed);
    got = ps2_vfs_ulz_decode(packed, pm->packed, orig, pm->unpacked);
    free(packed);
    if (got != (s64)pm->unpacked) {
        ps2_log("vfs: DATA.PAC member %u did not decode (%lld of %u bytes)", m,
                (long long)got, pm->unpacked);
        free(orig);
        free(new_offs);
        return -1;
    }
    size = pac_layout(pm, new_offs);
    buf = (u8 *)calloc(1, (size_t)size);
    if (!buf) ps2_fatal("vfs: out of memory composing an archive member");
    wr32(buf, pm->nfiles);
    for (u32 k = 0; k < pm->nfiles; k++) {
        const pac_over *ov;
        wr32(buf + 4u + 4u * k, new_offs[k]);
        if (!new_offs[k]) continue;
        ov = pac_override(pm, k);
        if (ov) claim_read(ov->claim, buf + new_offs[k]);
        else memcpy(buf + new_offs[k], orig + pm->offs[k], pac_extent(pm, k));
    }
    free(orig);
    free(new_offs);
    *out = buf;
    *len = (u32)size;
    return 0;
}

int ps2_vfs_pac_compose(u32 member, u8 **out, u32 *len) {
    int rc;
    pthread_mutex_lock(&vfs_lock);
    rc = compose_locked(member, out, len);
    pthread_mutex_unlock(&vfs_lock);
    return rc;
}

int ps2_vfs_pac_fill_guest(u32 member, u32 guest_addr, u32 capacity) {
    u8 *buf;
    u32 len;
    if (!ps2_vfs_pac_is_changed(member)) return -1;
    if (ps2_vfs_pac_compose(member, &buf, &len) != 0) return -1;
    if (len != pac[member].size || len > capacity) {
        ps2_log("vfs: DATA.PAC member %u came out at %u bytes against the %u "
                "the game allocated; not written", member, len, capacity);
        free(buf);
        return -1;
    }
    guest_put(guest_addr, buf, len);
    free(buf);
    pac_fills++;
    pac_fill_bytes += len;
    return (int)len;
}

static int resolve_file(const char *norm, u32 *member, u32 *index) {
    u32 first, count;
    if (!pac_node) return -1;
    if (iprefix(norm, PAC_PREFIX)) return parse_scoped(norm, member, index);
    count = names_find(norm, &first);
    if (!count) return -1;
    *member = pnames[pnames_sorted[first]].member;
    *index = pnames[pnames_sorted[first]].index;
    return 0;
}

static s64 archive_file_size(u32 m, u32 k) {
    const pac_member *pm = &pac[m];
    const pac_over *ov;
    if (pac_load_header(m) != 0 || k >= pm->nfiles) return -1;
    ov = pac_override(pm, k);
    if (ov) return (s64)ov->claim->size;
    return pm->offs[k] ? (s64)pac_extent(pm, k) : -1;
}

s64 ps2_vfs_stat(const char *path) {
    char norm[512];
    vfs_node *n;
    u32 m, k;
    s64 size = -1;
    if (!path || norm_path(path, norm, sizeof norm) <= 0 || !vfs_opened) return -1;
    pthread_mutex_lock(&vfs_lock);
    n = lookup(norm);
    if (n) size = n->f.size;
    else if (resolve_file(norm, &m, &k) == 0) size = archive_file_size(m, k);
    pthread_mutex_unlock(&vfs_lock);
    return size;
}

s64 ps2_vfs_read_path(const char *path, u64 pos, u32 len, void *dst) {
    char norm[512];
    vfs_node *n;
    u32 m, k, mlen;
    s64 size, got = -1;
    u8 *member;
    if (!path || norm_path(path, norm, sizeof norm) <= 0 || !vfs_opened) return -1;
    pthread_mutex_lock(&vfs_lock);
    n = lookup(norm);
    if (n) {
        if (pos >= n->f.size) got = 0;
        else {
            u64 left = (u64)n->f.size - pos;
            got = left < len ? (s64)left : (s64)len;
            node_read(n, pos, (u32)got, (u8 *)dst);
        }
    } else if (resolve_file(norm, &m, &k) == 0
               && (size = archive_file_size(m, k)) >= 0
               && compose_locked(m, &member, &mlen) == 0) {
        u32 at = rd32(member + 4u + 4u * k);
        if (pos >= (u64)size) got = 0;
        else {
            u64 left = (u64)size - pos;
            got = left < len ? (s64)left : (s64)len;
            memcpy(dst, member + at + pos, (size_t)got);
        }
        free(member);
    }
    pthread_mutex_unlock(&vfs_lock);
    return got;
}

void ps2_vfs_claim(const char *path, const char *host, u64 size, int priority,
                   const char *owner) {
    char norm[512];
    vfs_claim *c;
    if (vfs_opened) {
        ps2_log("vfs: %s's '%s' arrived after the disc was opened; ignored",
                owner ? owner : "?", path);
        return;
    }
    if (norm_path(path, norm, sizeof norm) <= 0) {
        ps2_log("vfs: %s's '%s' is not a usable path; ignored",
                owner ? owner : "?", path);
        return;
    }
    if (nclaims == cclaims) {
        cclaims = cclaims ? cclaims * 2u : 64u;
        claims = (vfs_claim **)realloc(claims, cclaims * sizeof *claims);
        if (!claims) ps2_fatal("vfs: out of memory recording a mod file");
    }
    c = (vfs_claim *)calloc(1, sizeof *c);
    if (!c) ps2_fatal("vfs: out of memory recording a mod file");
    c->path = xstrdup(norm);
    c->host = xstrdup(host);
    c->size = size;
    c->priority = priority;
    c->order = nclaims;
    c->owner = owner ? owner : "?";
    c->scoped = iprefix(norm, PAC_PREFIX);
    claims[nclaims++] = c;
}

static int claim_ptr_cmp(const void *a, const void *b) {
    const vfs_claim *x = *(const vfs_claim *const *)a;
    const vfs_claim *y = *(const vfs_claim *const *)b;
    int c = icmp(x->path, y->path);
    if (c) return c;
    return claim_beats(x, y) > 0 ? 1 : -1;
}

static void settle_file(vfs_claim **run, u32 i, u32 j, vfs_node *n) {
    vfs_claim *win = run[j - 1];
    claim_kind kind = n ? CL_DISC : CL_NEW;
    for (u32 x = i; x < j; x++) run[x]->kind = kind;
    for (u32 x = i; x + 1u < j; x++) {
        char what[300];
        snprintf(what, sizeof what, "'%s'", win->path);
        say_conflict(what, win, run[x]);
    }
    if (win->size > 0xFFFFFFFFull) {
        win->kind = CL_BAD;
        ps2_log("vfs: %s's '%s' is %llu bytes; a disc file cannot be over 4 GB",
                win->owner, win->path, (unsigned long long)win->size);
        return;
    }
    if (n) {
        ps2_log("vfs: %s replaces '%s' (%llu bytes, was %u)", win->owner,
                n->f.name, (unsigned long long)win->size, n->disc_size);
    } else {
        n = add_node(win->path, 0);
        if (!n) { win->kind = CL_BAD; return; }
        ps2_log("vfs: %s adds '%s', which is not a file the game has -- a new "
                "file that code mods can read.  If it was meant to replace one "
                "of the game's files, the name is wrong.", win->owner, win->path);
    }
    n->src = SRC_HOST;
    n->host = win->host;
    n->f.size = (u32)win->size;
    n->claim = win;
    win->won = 1;
}

static int is_archive_name(const char *path) {
    u32 first;
    return pac_node && names_find(path, &first) > 0;
}

static void resolve_claims(void) {
    vfs_claim **run, **pending;
    u32 *deferred, ndeferred = 0, npending = 0;
    run = (vfs_claim **)malloc((nclaims ? nclaims : 1u) * sizeof *run);
    pending = (vfs_claim **)malloc((nclaims ? nclaims : 1u) * sizeof *pending);
    deferred = (u32 *)malloc((nclaims ? nclaims : 1u) * 2u * sizeof *deferred);
    if (!run || !pending || !deferred) ps2_fatal("vfs: out of memory resolving mods");
    memcpy(run, claims, nclaims * sizeof *run);
    if (nclaims > 1) qsort(run, nclaims, sizeof *run, claim_ptr_cmp);

    for (u32 i = 0; i < nclaims;) {
        u32 j = i;
        vfs_node *n;
        while (j < nclaims && !icmp(run[j]->path, run[i]->path)) j++;
        n = run[i]->scoped ? NULL : lookup(run[i]->path);
        if (n) settle_file(run, i, j, n);
        else { deferred[2 * ndeferred] = i; deferred[2 * ndeferred + 1] = j; ndeferred++; }
        i = j;
    }

    pac_open();

    for (u32 d = 0; d < ndeferred; d++) {
        u32 i = deferred[2 * d], j = deferred[2 * d + 1];
        if (run[i]->scoped || is_archive_name(run[i]->path)) {
            for (u32 x = i; x < j; x++) pending[npending++] = run[x];
        } else {
            settle_file(run, i, j, NULL);
        }
    }
    if (npending) {
        if (pac_node) resolve_archive(pending, npending);
        else
            for (u32 i = 0; i < npending; i++) {
                pending[i]->kind = CL_BAD;
                ps2_log("vfs: %s's '%s' is an archive file, and this disc has "
                        "no DATA.PAC", pending[i]->owner, pending[i]->path);
            }
    }
    free(run);
    free(pending);
    free(deferred);
}

int ps2_vfs_open(const char *path) {
    u64 size;
    int is_dir;
    if (vfs_opened) return 0;
    if (!path || !*path || host_stat(path, &size, &is_dir) != 0) {
        ps2_log("vfs: no disc at '%s'", path ? path : "");
        return -1;
    }
    snprintf(vfs_path, sizeof vfs_path, "%s", path);
    if (is_dir) {
        vfs_is_tree = 1;
        tree_scan(path, "", 0);
    } else {
        u8 pvd[SECTOR];
        u32 root_lsn, root_size;
        image_fp = fopen(path, "rb");
        if (!image_fp || image_sector(16, pvd) != 0
            || memcmp(pvd + 1, "CD001", 5) != 0) {
            ps2_log("vfs: '%s' is not an ISO9660 image", path);
            if (image_fp) fclose(image_fp);
            image_fp = NULL;
            return -1;
        }
        memcpy(&root_lsn, pvd + 156 + 2, 4);
        memcpy(&root_size, pvd + 156 + 10, 4);
        image_scan_dir(root_lsn, root_size, "", 0);
    }
    if (!nnodes) {
        ps2_log("vfs: '%s' has no files", path);
        return -1;
    }
    resolve_claims();

    {
        u32 lsn = FIRST_LSN;
        for (u32 i = 0; i < nnodes; i++) {
            nodes[i]->f.lsn = lsn;
            lsn += sectors_of(nodes[i]->f.size);
        }
        end_lsn = lsn;
    }
    vfs_opened = 1;
    ps2_log("vfs: %s '%s', %u files, sectors %u..%u%s", vfs_is_tree
            ? "extracted tree" : "disc image", path, nnodes, FIRST_LSN, end_lsn,
            pac_node ? "" : " (no DATA.PAC)");
    return 0;
}

void ps2_vfs_owner_stats(const char *owner, u32 *files, s64 *heap_delta) {
    u32 n = 0;
    s64 d = 0;
    for (u32 i = 0; i < nclaims; i++) {
        if (!owner || strcmp(claims[i]->owner, owner)) continue;
        n += claims[i]->won;
        d += claims[i]->heap_delta;
    }
    if (files) *files = n;
    if (heap_delta) *heap_delta = d;
}

void ps2_vfs_report(void) {
    u32 replaced = 0, added = 0, bad = 0, archive_files = 0;
    s64 heap = 0;
    if (!vfs_opened) return;
    for (u32 i = 0; i < nclaims; i++) {
        const vfs_claim *c = claims[i];
        if (c->kind == CL_DISC && c->won) replaced++;
        else if (c->kind == CL_NEW && c->won) added++;
        else if (c->kind == CL_PAC) archive_files += c->won;
        else if (c->kind == CL_BAD) bad++;
        heap += c->heap_delta;
    }
    ps2_log("---- filesystem ----");
    ps2_log("   %s '%s': %u files, sectors %u..%u", vfs_is_tree
            ? "extracted tree" : "disc image", vfs_path, nnodes, FIRST_LSN,
            end_lsn);
    ps2_log("   %llu sector(s) and %llu byte(s) read, %llu read(s) from mod files",
            (unsigned long long)reads_sectors, (unsigned long long)reads_bytes,
            (unsigned long long)reads_from_mods);
    if (!nclaims) return;
    for (u32 i = 0; i < nnodes; i++)
        if (nodes[i]->claim)
            ps2_log("   %-8s %-32s %10u bytes  from %s",
                    nodes[i]->claim->kind == CL_NEW ? "new" : "replaced",
                    nodes[i]->f.name, nodes[i]->f.size, nodes[i]->claim->owner);
    ps2_log("   archive: %u file(s) in %u of %u member(s) changed, guest "
            "allocation %+lld bytes; %llu member load(s) served (%llu bytes)",
            archive_files, pac_nchanged, pac_count, (long long)heap,
            (unsigned long long)pac_fills, (unsigned long long)pac_fill_bytes);
    if (replaced || added)
        ps2_log("   %u whole file(s) replaced, %u new file(s)", replaced, added);
    if (nconflicts)
        ps2_log("   %u conflict(s) between mods, each named above", nconflicts);
    if (bad)
        ps2_log("   %u mod file(s) could not be placed, each named above", bad);
}
