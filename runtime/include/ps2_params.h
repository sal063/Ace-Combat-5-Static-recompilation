#ifndef PS2_PARAMS_H
#define PS2_PARAMS_H

#include "ps2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

int  ps2_params_set(const char *name, const char *value, int priority,
                    const char *owner);

int  ps2_params_load(const char *path, int priority, const char *owner);

void ps2_params_start(void);

void ps2_params_report(void);

#ifdef __cplusplus
}
#endif

#endif
