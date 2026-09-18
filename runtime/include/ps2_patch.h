#ifndef PS2_PATCH_H
#define PS2_PATCH_H

#include "ps2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { PS2_PATCH_ONCE = 0, PS2_PATCH_ALWAYS = 1, PS2_PATCH_BOTH = 2 };

#define PS2_PATCH_MAX_BYTES 256

int  ps2_patch_add(u32 addr, const u8 *bytes, const u8 *original, u32 len,
                   int when, int priority, const char *owner,
                   const char *origin);
int  ps2_patch_remove(int handle);

int  ps2_patch_load_pnach(const char *path, int priority, const char *owner);

void ps2_patch_start(void);

void ps2_patch_field(void);

void ps2_patch_report(void);

#ifdef __cplusplus
}
#endif

#endif
