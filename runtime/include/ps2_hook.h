#ifndef PS2_HOOK_H
#define PS2_HOOK_H

#include "ps2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOOK_MAX_CALL_DEPTH 256

typedef int (*ps2_hook_fn)(ps2_ctx *ctx, void *user);

int  ps2_hook_init(void);

int  ps2_hook_before(u32 guest_addr, ps2_hook_fn fn, void *user,
                     int priority, const char *owner);
int  ps2_hook_replace(u32 guest_addr, ps2_hook_fn fn, void *user,
                      int priority, const char *owner);
int  ps2_hook_after(u32 guest_addr, ps2_hook_fn fn, void *user,
                    int priority, const char *owner);

int  ps2_hook_redirect(u32 from_guest, u32 to_guest, int priority,
                       const char *owner);

int  ps2_hook_remove(int handle);

void ps2_hook_call_original(u32 guest_addr, ps2_ctx *ctx);

u32  ps2_hook_entry_arg(int n, int *ok);

void ps2_hook_report(void);

#ifdef __cplusplus
}
#endif

#endif
