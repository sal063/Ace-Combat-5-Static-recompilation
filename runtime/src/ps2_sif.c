#include "ps2_runtime.h"
#include <stdio.h>
#include <string.h>

#define SIF_MSCOM  0x1000F200u
#define SIF_SMCOM  0x1000F210u
#define SIF_MSFLG  0x1000F220u
#define SIF_SMFLG  0x1000F230u
#define SIF_CTRL   0x1000F240u
#define SIF_BD6    0x1000F260u

#define SIF_STAT_SIFINIT   0x10000u
#define SIF_STAT_CMDINIT   0x20000u
#define SIF_STAT_BOOTEND   0x40000u

static u32 sif_mscom, sif_smcom, sif_msflg, sif_smflg, sif_ctrl, sif_bd6;
static u32 sif_regs[32];
static u64 sif_rpc_calls;

void ps2_sif_init(void) {
    sif_smflg = SIF_STAT_SIFINIT | SIF_STAT_CMDINIT | SIF_STAT_BOOTEND;
    sif_msflg = SIF_STAT_SIFINIT | SIF_STAT_CMDINIT | SIF_STAT_BOOTEND;
    sif_ctrl = 0xF0000102u;
    sif_bd6 = 0;
    memset(sif_regs, 0, sizeof(sif_regs));
    sif_regs[1] = 0x00000000u;
    sif_regs[2] = PS2_IOP_RAM_BASE;
    sif_regs[3] = SIF_STAT_SIFINIT | SIF_STAT_CMDINIT | SIF_STAT_BOOTEND;
    sif_regs[4] = SIF_STAT_SIFINIT | SIF_STAT_CMDINIT | SIF_STAT_BOOTEND;
}

u32 ps2_sif_reg_read(u32 addr) {
    switch (addr) {
    case SIF_MSCOM: return sif_mscom;
    case SIF_SMCOM: return sif_smcom;
    case SIF_MSFLG: return sif_msflg;
    case SIF_SMFLG: return sif_smflg;
    case SIF_CTRL:  return sif_ctrl;
    case SIF_BD6:   return sif_bd6;
    default: return 0;
    }
}

void ps2_sif_reg_write(u32 addr, u32 val) {
    switch (addr) {
    case SIF_MSCOM: sif_mscom = val; break;
    case SIF_SMCOM: sif_smcom = val; break;
    case SIF_MSFLG: sif_msflg |= val; break;
    case SIF_SMFLG: sif_smflg &= ~val; break;
    case SIF_CTRL:  sif_ctrl = (sif_ctrl & ~val) | 0xF0000002u; break;
    case SIF_BD6:   sif_bd6 = val; break;
    default: break;
    }
}

void ps2_sif_set_reg(u32 reg, u32 val) {
    sif_regs[reg & 31] = val;
}

u32 ps2_sif_get_reg(u32 reg) {
    return sif_regs[reg & 31];
}

void ps2_sif_stats(u64 *rpc_calls) { *rpc_calls = sif_rpc_calls; }
