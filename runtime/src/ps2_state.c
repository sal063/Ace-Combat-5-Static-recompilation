#include "ps2_runtime.h"
#include "ps2_hle.h"
#include <stdio.h>
#include <string.h>

#define PS2_SNAP_MAGIC "PS2SNAP1"

typedef struct { FILE *fp; u32 chunks; u64 bytes; } snap_out;

static void snap_put(void *ud, const char *tag, const void *p, u64 n) {
    snap_out *o = (snap_out *)ud;
    char t[16];
    if (!o->fp) return;
    memset(t, 0, sizeof t);
    memcpy(t, tag, strlen(tag) < 15u ? strlen(tag) : 15u);
    fwrite(t, 1, 16, o->fp);
    fwrite(&n, sizeof n, 1, o->fp);
    if (n && p) fwrite(p, 1, (size_t)n, o->fp);
    o->chunks++;
    o->bytes += n;
}

static const char *dump_path;

void ps2_state_dump_arm(const char *path) { dump_path = path; }

static volatile int dump_request;

void ps2_state_request_dump(void) { dump_request = 1; }
int  ps2_state_dump_pending(void) { return dump_request; }

void ps2_state_dump_numbered(const char *note) {
    static unsigned seq;
    char base[400], path[512];
    size_t n;
    const char *src = dump_path ? dump_path : "ps2_state";
    dump_request = 0;
    n = strlen(src);
    if (n >= sizeof base) n = sizeof base - 1u;
    memcpy(base, src, n);
    base[n] = 0;
    if (n > 4u && !strcmp(base + n - 4u, ".bin")) base[n - 4u] = 0;
    snprintf(path, sizeof path, "%s_%03u.bin", base, ++seq);
    if (note) ps2_log("state: capture %u -- %s", seq, note);
    ps2_state_dump(path);
}

void ps2_state_dump_if_armed(void) {
    if (dump_path) ps2_state_dump(dump_path);
}

void ps2_state_dump(const char *path) {
    snap_out o;
    o.fp = fopen(path, "wb");
    o.chunks = 0;
    o.bytes = 0;
    if (!o.fp) {
        ps2_log("state: cannot open '%s' for writing", path);
        return;
    }
    fwrite(PS2_SNAP_MAGIC, 1, 8, o.fp);
    snap_put(&o, "ram", ps2_ram, ps2_ram ? PS2_RAM_SIZE : 0);
    snap_put(&o, "spr", ps2_spr, ps2_spr ? PS2_SPR_SIZE : 0);
    snap_put(&o, "iop_ram", ps2_iop_ram, ps2_iop_ram ? PS2_IOP_RAM_SIZE : 0);
    ps2_vu_state_save(snap_put, &o);
    ps2_gs_state_save(snap_put, &o);
    ps2_hw_state_save(snap_put, &o);
    ps2_kernel_state_save(snap_put, &o);
    ps2_ipu_state_save(snap_put, &o);
    ps2_vk_state_save(snap_put, &o);
    fclose(o.fp);
    ps2_log("state: wrote '%s' -- %u chunks, %llu bytes", path, o.chunks,
            (unsigned long long)o.bytes);
}
