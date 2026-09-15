#ifndef _WIN32
#define _FILE_OFFSET_BITS 64
#endif
#include "ps2_runtime.h"
#include "ps2_hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#define ISO_SECTOR 2048

static FILE *iso_fp;
static char  iso_root[512];
static int   iso_is_tree;

static ps2_disc_file *iso_files;
static u32 iso_nfiles, iso_cap;
static u32 iso_root_lsn, iso_root_size;
static u32 iso_next_lsn = 32;

static FILE *tree_fp;
static u32   tree_fp_index = 0xFFFFFFFFu;

int ps2_iso_ready(void) { return iso_fp != NULL || iso_is_tree; }

static ps2_disc_file *iso_add(const char *name, u32 lsn, u32 size,
                              const u8 *date) {
    ps2_disc_file *f;
    if (iso_nfiles == iso_cap) {
        iso_cap = iso_cap ? iso_cap * 2 : 256;
        iso_files = (ps2_disc_file *)realloc(iso_files,
                                             iso_cap * sizeof(ps2_disc_file));
        if (!iso_files) ps2_fatal("out of memory building the disc index");
    }
    f = &iso_files[iso_nfiles++];
    memset(f, 0, sizeof(*f));
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->lsn = lsn;
    f->size = size;
    if (date) memcpy(f->date, date, 8);
    return f;
}

static int disc_seek(FILE *fp, u64 offset) {
#ifdef _WIN32
    return _fseeki64(fp, (s64)offset, SEEK_SET);
#else
    return fseeko(fp, (off_t)offset, SEEK_SET);
#endif
}

static int iso_read_sector(u32 lsn, void *dst) {
    if (!iso_fp) return -1;
    if (disc_seek(iso_fp, (u64)lsn * ISO_SECTOR) != 0)
        return -1;
    return fread(dst, 1, ISO_SECTOR, iso_fp) == ISO_SECTOR ? 0 : -1;
}

static void iso_scan_dir(u32 lsn, u32 size, const char *prefix, int depth) {
    u8 *buf = (u8 *)malloc(ISO_SECTOR);
    u32 sectors = (size + ISO_SECTOR - 1) / ISO_SECTOR;
    if (!buf) return;
    for (u32 s = 0; s < sectors; s++) {
        u32 off = 0;
        if (iso_read_sector(lsn + s, buf) != 0) break;
        while (off < ISO_SECTOR) {
            u8 len = buf[off];
            u32 elsn, esize, nlen;
            char name[128], full[256];
            if (len == 0) break;
            if (off + len > ISO_SECTOR) break;
            memcpy(&elsn, buf + off + 2, 4);
            memcpy(&esize, buf + off + 10, 4);
            nlen = buf[off + 32];
            if (nlen == 1 && (buf[off + 33] == 0 || buf[off + 33] == 1)) {
                off += len;
                continue;
            }
            if (nlen > sizeof(name) - 1) nlen = sizeof(name) - 1;
            memcpy(name, buf + off + 33, nlen);
            name[nlen] = 0;
            for (u32 i = 0; i < nlen; i++)
                if (name[i] == ';') { name[i] = 0; break; }
            snprintf(full, sizeof(full), "%.120s%s%.120s", prefix,
                     *prefix ? "/" : "", name);
            if (buf[off + 25] & 2) {
                if (depth < 6) iso_scan_dir(elsn, esize, full, depth + 1);
            } else {
                iso_add(full, elsn, esize, buf + off + 18);
            }
            off += len;
        }
    }
    free(buf);
}

static void tree_scan(const char *hostdir, const char *prefix, int depth) {
    DIR *d = opendir(hostdir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        char hp[1024], full[256];
        struct stat st;
        if (e->d_name[0] == '.') continue;
        snprintf(hp, sizeof(hp), "%s/%s", hostdir, e->d_name);
        if (stat(hp, &st) != 0) continue;
        snprintf(full, sizeof(full), "%.120s%s%.120s", prefix,
                 *prefix ? "/" : "", e->d_name);
        if (S_ISDIR(st.st_mode)) {
            if (depth < 6) tree_scan(hp, full, depth + 1);
        } else {
            u32 size = (u32)st.st_size;
            u32 sect = (size + ISO_SECTOR - 1) / ISO_SECTOR;
            iso_add(full, iso_next_lsn, size, NULL);
            iso_next_lsn += sect ? sect : 1;
        }
    }
    closedir(d);
}

static ps2_disc_file *tree_locate(u32 lsn, u32 *sector_in_file) {
    for (u32 i = 0; i < iso_nfiles; i++) {
        u32 sect = (iso_files[i].size + ISO_SECTOR - 1) / ISO_SECTOR;
        if (!sect) sect = 1;
        if (lsn >= iso_files[i].lsn && lsn < iso_files[i].lsn + sect) {
            *sector_in_file = lsn - iso_files[i].lsn;
            tree_fp_index = i;
            return &iso_files[i];
        }
    }
    return NULL;
}

static u32 tree_open_index = 0xFFFFFFFFu;

static FILE *tree_handle(u32 index) {
    char hp[1024];
    if (tree_fp && tree_open_index == index) return tree_fp;
    if (tree_fp) fclose(tree_fp);
    snprintf(hp, sizeof(hp), "%s/%s", iso_root, iso_files[index].name);
    tree_fp = fopen(hp, "rb");
    tree_open_index = tree_fp ? index : 0xFFFFFFFFu;
    if (!tree_fp) ps2_log("disc: cannot open '%s'", hp);
    return tree_fp;
}

int ps2_iso_open(const char *path) {
    struct stat st;
    u8 *pvd;
    if (!path || !*path) return -1;

    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(iso_root, sizeof(iso_root), "%s", path);
        iso_is_tree = 1;
        tree_scan(path, "", 0);
        ps2_log("disc: extracted tree '%s', %u files indexed, %u synthetic "
                "sectors", path, iso_nfiles, iso_next_lsn);
        return 0;
    }

    iso_fp = fopen(path, "rb");
    if (!iso_fp) {
        ps2_log("disc: cannot open '%s'", path);
        return -1;
    }
    pvd = (u8 *)malloc(ISO_SECTOR);
    if (!pvd) return -1;
    if (iso_read_sector(16, pvd) != 0 || memcmp(pvd + 1, "CD001", 5) != 0) {
        ps2_log("disc: '%s' has no ISO9660 primary volume descriptor", path);
        free(pvd);
        fclose(iso_fp);
        iso_fp = NULL;
        return -1;
    }
    memcpy(&iso_root_lsn, pvd + 156 + 2, 4);
    memcpy(&iso_root_size, pvd + 156 + 10, 4);
    free(pvd);
    iso_scan_dir(iso_root_lsn, iso_root_size, "", 0);
    ps2_log("disc: image '%s' root at lsn=%u, %u files indexed",
            path, iso_root_lsn, iso_nfiles);
    return 0;
}

const ps2_disc_file *ps2_iso_find(const char *name) {
    char norm[128];
    const char *p = name;
    u32 n = 0;
    if (!name) return NULL;
    for (const char *q = p; *q && (u32)(q - p) < 16u; q++) {
        if (*q == '/' || *q == '\\') break;
        if (*q == ':') { p = q + 1; break; }
    }
    while (*p && n < sizeof(norm) - 1) {
        char c = *p++;
        if (c == '\\') c = '/';
        if (c == ';') break;
        norm[n++] = c;
    }
    norm[n] = 0;
    p = norm;
    while (*p == '/') p++;
    for (u32 i = 0; i < iso_nfiles; i++) {
        const char *a = p, *b = iso_files[i].name;
        int eq = 1;
        while (*a && *b) {
            char ca = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a;
            char cb = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
            if (ca != cb) { eq = 0; break; }
            a++; b++;
        }
        if (eq && !*a && !*b) return &iso_files[i];
    }
    return NULL;
}

int ps2_iso_read_host(u32 lsn, u32 sectors, void *dst) {
    u8 *out = (u8 *)dst;
    if (iso_is_tree) {
        u32 done = 0;
        while (done < sectors) {
            u32 sif;
            ps2_disc_file *f = tree_locate(lsn + done, &sif);
            FILE *fp;
            size_t got;
            u32 run, avail;
            if (!f) {
                memset(out + (size_t)done * ISO_SECTOR, 0,
                       (size_t)(sectors - done) * ISO_SECTOR);
                return (int)sectors;
            }
            fp = tree_handle(tree_fp_index);
            if (!fp) return -1;
            avail = ((f->size + ISO_SECTOR - 1) / ISO_SECTOR) - sif;
            run = sectors - done < avail ? sectors - done : avail;
            if (disc_seek(fp, (u64)sif * ISO_SECTOR) != 0)
                return -1;
            memset(out + (size_t)done * ISO_SECTOR, 0,
                   (size_t)run * ISO_SECTOR);
            got = fread(out + (size_t)done * ISO_SECTOR, 1,
                        (size_t)run * ISO_SECTOR, fp);
            (void)got;
            done += run;
        }
        return (int)sectors;
    }
    if (!sectors) return 0;
    if (!iso_fp || disc_seek(iso_fp, (u64)lsn * ISO_SECTOR) != 0) return -1;
    if (fread(out, ISO_SECTOR, sectors, iso_fp) != sectors) return -1;
    return (int)sectors;
}

int ps2_iso_read(u32 lsn, u32 sectors, u32 guest_addr) {
    u8 *buf;
    int rc;
    if (!ps2_iso_ready()) return -1;
    buf = (u8 *)malloc((size_t)sectors * ISO_SECTOR);
    if (!buf) return -1;
    rc = ps2_iso_read_host(lsn, sectors, buf);
    if (rc > 0) ps2_put_mem(guest_addr, buf, (size_t)sectors * ISO_SECTOR);
    free(buf);
    return rc;
}

int ps2_iso_read_file(const ps2_disc_file *f, u64 pos, u32 len, u32 guest_addr) {
    static u8 buf[ISO_SECTOR * 16];
    u32 done = 0;
    if (!f || !ps2_iso_ready()) return -1;
    if (pos >= f->size) return 0;
    if (pos + len > f->size) len = (u32)((u64)f->size - pos);
    while (done < len) {
        u64 at = pos + done;
        u32 lsn = f->lsn + (u32)(at / ISO_SECTOR);
        u32 skip = (u32)(at % ISO_SECTOR);
        u32 want = len - done, sectors, got, i;
        sectors = (skip + want + ISO_SECTOR - 1) / ISO_SECTOR;
        if (sectors > 16) sectors = 16;
        if (ps2_iso_read_host(lsn, sectors, buf) <= 0) break;
        got = sectors * ISO_SECTOR - skip;
        if (got > want) got = want;
        for (i = 0; i < got; i++) ps2_w8(guest_addr + done + i, buf[skip + i]);
        done += got;
    }
    return (int)done;
}

u32 ps2_iso_file_count(void) { return iso_nfiles; }

void ps2_iso_dump(u32 max) {
    for (u32 i = 0; i < iso_nfiles && i < max; i++)
        ps2_log("   %-32s lsn=%-8u size=%u",
                iso_files[i].name, iso_files[i].lsn, iso_files[i].size);
}
