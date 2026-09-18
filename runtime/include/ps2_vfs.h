#ifndef PS2_VFS_H
#define PS2_VFS_H

#include "ps2_runtime.h"
#include "ps2_hle.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PS2_VFS_SECTOR 2048u

void ps2_vfs_claim(const char *path, const char *host, u64 size,
                   int priority, const char *owner);

int  ps2_vfs_open(const char *disc_path);
int  ps2_vfs_ready(void);

const ps2_disc_file *ps2_vfs_find(const char *name);
u32  ps2_vfs_file_count(void);

int  ps2_vfs_read_sectors(u32 lsn, u32 sectors, void *dst);
int  ps2_vfs_read_sectors_guest(u32 lsn, u32 sectors, u32 guest_addr);

int  ps2_vfs_read(const ps2_disc_file *f, u64 pos, u32 len, void *dst);
int  ps2_vfs_read_guest(const ps2_disc_file *f, u64 pos, u32 len,
                        u32 guest_addr);

u32  ps2_vfs_pac_changed(void);
int  ps2_vfs_pac_is_changed(u32 member);

u32  ps2_vfs_pac_size(u32 member);

int  ps2_vfs_pac_fill_guest(u32 member, u32 guest_addr, u32 capacity);

int  ps2_vfs_pac_compose(u32 member, u8 **out, u32 *len);

void ps2_vfs_pac_revert(const char *why);

s64  ps2_vfs_stat(const char *path);
s64  ps2_vfs_read_path(const char *path, u64 pos, u32 len, void *dst);

void ps2_vfs_owner_stats(const char *owner, u32 *files, s64 *heap_delta);
void ps2_vfs_report(void);

s64  ps2_vfs_ulz_decode(const u8 *src, u32 len, u8 *dst, u32 cap);

#ifdef __cplusplus
}
#endif

#endif
