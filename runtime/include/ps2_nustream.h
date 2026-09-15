#pragma once
#include "ps2_runtime.h"
void ps2_nustream_command(u32 cmd, const u32 *args, const char *name);
void ps2_nustream_status(u32 slot, u32 *position, u32 *ended);
void ps2_nustream_report(u32 slot);
u64 ps2_nustream_voice_mask(void);
void ps2_nustream_mix(s16 *out, u32 frames, const s16 volumes[48][2]);
