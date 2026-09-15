#ifndef PS2_STATECAP_H
#define PS2_STATECAP_H

#include "ps2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

void ps2_statecap_init(u8 *ram, u64 size);

void ps2_statecap_register(const char *name, void (*fn)(const char *dir));

void ps2_statecap_request(const char *why);

int         ps2_statecap_gs_pending(void);
const char *ps2_statecap_dir(void);
void        ps2_statecap_gs_done(void);

void        ps2_gs_statecap(const char *dir);

#ifdef __cplusplus
}
#endif

#endif
