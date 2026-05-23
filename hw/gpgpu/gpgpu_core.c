/*
 * QEMU GPGPU - RISC-V SIMT Core Implementation
 *
 * Copyright (c) 2024-2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "gpgpu.h"
#include "gpgpu_core.h"

/*
 * ============================================================================
 * Low-precision float conversion helpers
 * ============================================================================
 */

/* Float32 → BF16 (truncate mantissa to 7 bits) */
static uint16_t f32_to_bf16(float val)
{
    uint32_t bits;
    memcpy(&bits, &val, 4);
    uint32_t sign = (bits >> 16) & 0x8000;
    int32_t exp = ((bits >> 23) & 0xFF);
    uint32_t mantissa = (bits >> 16) & 0x7F;

    if (exp == 0xFF) {
        /* Inf/NaN: saturate to max finite or NaN */
        return sign | 0x7F80 | (mantissa ? 0x0040 : 0);
    }
    if (exp == 0 && mantissa == 0) {
        return sign;
    }
    /* Round: add 0x7FFF + ((mantissa >> 7) & 1) before truncating */
    uint32_t rounding_bias = 0x7FFF + ((bits >> 16) & 1);
    bits += rounding_bias;
    return ((bits >> 16) & 0xFFFF);
}

/* BF16 → Float32 (zero-extend mantissa) */
static float bf16_to_f32(uint16_t val)
{
    uint32_t bits32 = (uint32_t)val << 16;
    float result;
    memcpy(&result, &bits32, 4);
    return result;
}

/* Float32 → FP8 E4M3 (1 sign, 4 exp biased 7, 3 mantissa) */
static uint8_t f32_to_e4m3(float val)
{
    uint32_t bits;
    memcpy(&bits, &val, 4);
    uint32_t sign = (bits >> 31) & 1;
    int32_t exp = ((bits >> 23) & 0xFF) - 127;
    uint32_t mantissa = bits & 0x7FFFFF;

    if ((bits & 0x7FFFFFFF) == 0) {
        return sign << 7;  /* ±0 */
    }
    if ((bits & 0x7F800000) == 0x7F800000) {
        if (mantissa) {
            return (sign << 7) | 0x7F;  /* NaN */
        }
        /* Inf: saturate to max finite (448) */
        return (sign << 7) | 0x7E;
    }

    exp += 7;  /* Rebias: E4M3 bias=7, float32 bias=127, so +7 */

    if (exp <= 0) {
        /* Subnormal or underflow → 0 */
        if (exp < -2) return sign << 7;
        mantissa = (mantissa | 0x800000) >> (1 - exp + 23 - 3);
        return (sign << 7) | (mantissa & 0x07);
    }
    if (exp >= 0x0F) {
        /* Overflow: saturate to max (448) */
        return (sign << 7) | 0x7E;
    }

    return (sign << 7) | (exp << 3) | ((mantissa >> 20) & 0x07);
}

/* FP8 E4M3 → Float32 */
static float e4m3_to_f32(uint8_t val)
{
    uint32_t sign = (val >> 7) & 1;
    uint32_t exp = (val >> 3) & 0x0F;
    uint32_t mantissa = val & 0x07;
    uint32_t bits;

    if (exp == 0) {
        if (mantissa == 0) {
            bits = sign << 31;
        } else {
            /* Subnormal: value = (-1)^s * 2^(1-7) * (m/8) = m * 2^-9 */
            bits = (sign << 31) | ((mantissa << 20) >> 1);
        }
    } else if (exp == 0x0F && mantissa == 0x07) {
        /* NaN */
        bits = 0x7FC00000 | (sign << 31);
    } else {
        bits = (sign << 31) | ((exp + 127 - 7) << 23) | (mantissa << 20);
    }

    float result;
    memcpy(&result, &bits, 4);
    return result;
}

/* Float32 → FP8 E5M2 (1 sign, 5 exp biased 15, 2 mantissa) */
static uint8_t f32_to_e5m2(float val)
{
    uint32_t bits;
    memcpy(&bits, &val, 4);
    uint32_t sign = (bits >> 31) & 1;
    int32_t exp = ((bits >> 23) & 0xFF) - 127;
    uint32_t mantissa = bits & 0x7FFFFF;

    if ((bits & 0x7FFFFFFF) == 0) {
        return sign << 7;
    }
    if ((bits & 0x7F800000) == 0x7F800000) {
        if (mantissa) return (sign << 7) | 0x7F;  /* NaN */
        /* Inf: saturate */
        return (sign << 7) | 0x7C;
    }

    exp += 15;  /* Rebias */

    if (exp <= 0) {
        if (exp < -1) return sign << 7;
        mantissa = (mantissa | 0x800000) >> (1 - exp + 23 - 2);
        return (sign << 7) | (mantissa & 0x03);
    }
    if (exp >= 0x1F) {
        return (sign << 7) | 0x7C;
    }

    return (sign << 7) | (exp << 2) | ((mantissa >> 21) & 0x03);
}

/* FP8 E5M2 → Float32 */
static float e5m2_to_f32(uint8_t val)
{
    uint32_t sign = (val >> 7) & 1;
    uint32_t exp = (val >> 2) & 0x1F;
    uint32_t mantissa = val & 0x03;
    uint32_t bits;

    if (exp == 0) {
        if (mantissa == 0) {
            bits = sign << 31;
        } else {
            bits = (sign << 31) | ((mantissa << 21) >> 2);
        }
    } else if (exp == 0x1F && mantissa != 0) {
        bits = 0x7FC00000 | (sign << 31);
    } else {
        bits = (sign << 31) | ((exp + 127 - 15) << 23) | (mantissa << 21);
    }

    float result;
    memcpy(&result, &bits, 4);
    return result;
}

/* Float32 → FP4 E2M1 (1 sign, 2 exp biased 1, 1 mantissa)
 * E2M1 codes: 0=0, 1=0.5, 2=1, 3=1.5, 4=2, 5=3, 6=4, 7=6
 * Rounding: round-to-nearest-even at midpoints */
static uint8_t f32_to_e2m1(float val)
{
    uint32_t bits;
    memcpy(&bits, &val, 4);
    uint32_t sign = (bits >> 31) & 1;

    if ((bits & 0x7FFFFFFF) == 0) {
        return sign << 3;  /* ±0 */
    }
    if ((bits & 0x7F800000) == 0x7F800000) {
        uint32_t mantissa = bits & 0x7FFFFF;
        if (mantissa) return (sign << 3) | 0x07;  /* NaN */
        return (sign << 3) | 0x06;  /* Inf → max */
    }

    uint32_t absbits = bits & 0x7FFFFFFF;
    float absval;
    memcpy(&absval, &absbits, 4);

    /* Thresholds at midpoints between representable values:
     * [0, 0.25) → 0, [0.25, 0.75) → 0.5, [0.75, 1.25) → 1,
     * [1.25, 1.75) → 1.5, [1.75, 2.5) → 2, [2.5, 3.5) → 3,
     * [3.5, 5.0) → 4, [5.0, ∞) → 6 */
    if (absval >= 5.0f) return (sign << 3) | 0x07;  /* → 6 */
    if (absval >= 3.5f) return (sign << 3) | 0x06;  /* → 4 */
    if (absval >= 2.5f) return (sign << 3) | 0x05;  /* → 3 */
    if (absval >= 1.75f) return (sign << 3) | 0x04; /* → 2 */
    if (absval >= 1.25f) return (sign << 3) | 0x03; /* → 1.5 */
    if (absval >= 0.75f) return (sign << 3) | 0x02; /* → 1 */
    if (absval >= 0.25f) return (sign << 3) | 0x01; /* → 0.5 */
    return sign << 3;  /* → 0 */
}

/* FP4 E2M1 → Float32 */
static float e2m1_to_f32(uint8_t val)
{
    uint32_t sign = (val >> 3) & 1;
    uint32_t exp = (val >> 1) & 0x03;
    uint32_t mantissa = val & 0x01;
    uint32_t bits;

    if (exp == 0) {
        if (mantissa == 0) {
            bits = sign << 31;
        } else {
            /* Subnormal: 0.5 * 2^(1-1) = 0.25 */
            bits = (sign << 31) | 0x3E800000;
        }
    } else {
        bits = (sign << 31) | ((exp + 127 - 1) << 23) | (mantissa << 22);
    }

    float result;
    memcpy(&result, &bits, 4);
    return result;
}

/*
 * ============================================================================
 * Warp initialization
 * ============================================================================
 */
void gpgpu_core_init_warp(GPGPUWarp *warp, uint32_t pc,
                          uint32_t thread_id_base, const uint32_t block_id[3],
                          uint32_t num_threads,
                          uint32_t warp_id, uint32_t block_id_linear)
{
    memset(warp, 0, sizeof(*warp));

    warp->thread_id_base = thread_id_base;
    warp->warp_id = warp_id;
    warp->block_id[0] = block_id[0];
    warp->block_id[1] = block_id[1];
    warp->block_id[2] = block_id[2];
    warp->active_mask = 0;

    for (uint32_t i = 0; i < GPGPU_WARP_SIZE; i++) {
        GPGPULane *lane = &warp->lanes[i];
        memset(lane, 0, sizeof(*lane));
        lane->pc = pc;
        lane->gpr[0] = 0;  /* x0 is always 0 */

        if (i < num_threads) {
            lane->active = true;
            warp->active_mask |= (1u << i);
            /* Encode mhartid: block[31:13] | warp[12:5] | tid[4:0] */
            lane->mhartid = MHARTID_ENCODE(block_id_linear, warp_id, i);
            set_float_rounding_mode(float_round_to_zero, &lane->fp_status);
        } else {
            lane->active = false;
        }
    }
}

/*
 * ============================================================================
 * VRAM access helpers
 * ============================================================================
 */
static inline uint32_t vram_read32(GPGPUState *s, uint32_t addr)
{
    if (addr + 4 > s->vram_size) {
        return 0;
    }
    uint32_t val;
    memcpy(&val, s->vram_ptr + addr, 4);
    return val;
}

static inline void vram_write32(GPGPUState *s, uint32_t addr, uint32_t val)
{
    if (addr + 4 > s->vram_size) {
        return;
    }
    memcpy(s->vram_ptr + addr, &val, 4);
}

/*
 * ============================================================================
 * Warp execution: RV32I + RV32F + Low-precision FP interpreter
 * ============================================================================
 */
int gpgpu_core_exec_warp(GPGPUState *s, GPGPUWarp *warp, uint32_t max_cycles)
{
    uint32_t cycle = 0;

    while (cycle < max_cycles) {
        /* Check if any lane is still active */
        bool any_active = false;
        for (uint32_t i = 0; i < GPGPU_WARP_SIZE; i++) {
            if (warp->lanes[i].active) {
                any_active = true;
                break;
            }
        }
        if (!any_active) {
            break;
        }

        /* Fetch instruction from VRAM (using lane 0's PC as reference) */
        uint32_t pc = 0;
        for (uint32_t i = 0; i < GPGPU_WARP_SIZE; i++) {
            if (warp->lanes[i].active) {
                pc = warp->lanes[i].pc;
                break;
            }
        }

        uint32_t insn = vram_read32(s, pc);

        /* Decode common fields */
        uint32_t opcode = insn & 0x7F;
        uint32_t rd = (insn >> 7) & 0x1F;
        uint32_t funct3 = (insn >> 12) & 0x07;
        uint32_t rs1 = (insn >> 15) & 0x1F;
        uint32_t rs2 = (insn >> 20) & 0x1F;
        uint32_t funct7 = (insn >> 25) & 0x7F;

        /* Check for ebreak */
        if (insn == 0x00100073) {
            /* ebreak: deactivate all lanes */
            for (uint32_t i = 0; i < GPGPU_WARP_SIZE; i++) {
                warp->lanes[i].active = false;
            }
            warp->active_mask = 0;
            return 0;
        }

        /* Execute on all active lanes (SIMT lockstep) */
        for (uint32_t i = 0; i < GPGPU_WARP_SIZE; i++) {
            GPGPULane *lane = &warp->lanes[i];
            if (!lane->active) {
                continue;
            }

            /* Ensure x0 is always 0 */
            lane->gpr[0] = 0;

            switch (opcode) {
            case 0x37: { /* LUI */
                int32_t imm = (int32_t)(insn & 0xFFFFF000);
                if (rd != 0) lane->gpr[rd] = (uint32_t)imm;
                lane->pc += 4;
                break;
            }
            case 0x17: { /* AUIPC */
                int32_t imm = (int32_t)(insn & 0xFFFFF000);
                if (rd != 0) lane->gpr[rd] = lane->pc + (uint32_t)imm;
                lane->pc += 4;
                break;
            }
            case 0x13: { /* OP-IMM: ADDI, ANDI, SLLI */
                int32_t imm_i = (int32_t)insn >> 20;
                uint32_t uimm_i = (uint32_t)imm_i;
                switch (funct3) {
                case 0x0: /* ADDI */
                    if (rd != 0) lane->gpr[rd] = lane->gpr[rs1] + uimm_i;
                    break;
                case 0x1: /* SLLI */
                    if (rd != 0) lane->gpr[rd] = lane->gpr[rs1] << (uimm_i & 0x1F);
                    break;
                case 0x7: /* ANDI */
                    if (rd != 0) lane->gpr[rd] = lane->gpr[rs1] & uimm_i;
                    break;
                default:
                    /* Unknown OP-IMM, NOP */
                    break;
                }
                lane->pc += 4;
                break;
            }
            case 0x33: { /* OP: ADD */
                switch (funct3) {
                case 0x0: /* ADD (funct7=0) or SUB (funct7=0x20) */
                    if (funct7 == 0x20) {
                        if (rd != 0) lane->gpr[rd] = lane->gpr[rs1] - lane->gpr[rs2];
                    } else {
                        if (rd != 0) lane->gpr[rd] = lane->gpr[rs1] + lane->gpr[rs2];
                    }
                    break;
                default:
                    break;
                }
                lane->pc += 4;
                break;
            }
            case 0x23: { /* STORE: SB, SH, SW */
                int32_t imm_s = ((int32_t)(insn & 0xFE000000) >> 20)
                                | ((insn >> 7) & 0x1F);
                uint32_t addr = lane->gpr[rs1] + (uint32_t)imm_s;
                switch (funct3) {
                case 0x2: /* SW */
                    vram_write32(s, addr, lane->gpr[rs2]);
                    break;
                default:
                    break;
                }
                lane->pc += 4;
                break;
            }
            case 0x03: { /* LOAD: LB, LH, LW, LBU, LHU */
                int32_t imm_i = (int32_t)insn >> 20;
                uint32_t addr = lane->gpr[rs1] + (uint32_t)imm_i;
                switch (funct3) {
                case 0x2: /* LW */
                    if (rd != 0) lane->gpr[rd] = vram_read32(s, addr);
                    break;
                default:
                    break;
                }
                lane->pc += 4;
                break;
            }
            case 0x73: { /* SYSTEM: CSRRS etc. */
                uint32_t csr = insn >> 20;
                switch (funct3) {
                case 0x2: { /* CSRRS */
                    uint32_t csr_val = 0;
                    if (csr == CSR_MHARTID) {
                        csr_val = lane->mhartid;
                    }
                    if (rd != 0) lane->gpr[rd] = csr_val;
                    break;
                }
                default:
                    break;
                }
                lane->pc += 4;
                break;
            }
            case 0x53: { /* OP-FP: RV32F and custom LP conversions */
                if (funct7 == 0x00) {
                    /* FADD.S (fmt=0, funct7[1:0]=00) */
                    float fa, fb, fr;
                    memcpy(&fa, &lane->fpr[rs1], 4);
                    memcpy(&fb, &lane->fpr[rs2], 4);
                    fr = fa + fb;
                    memcpy(&lane->fpr[rd], &fr, 4);
                } else if (funct7 == 0x08) {
                    /* FMUL.S */
                    float fa, fb, fr;
                    memcpy(&fa, &lane->fpr[rs1], 4);
                    memcpy(&fb, &lane->fpr[rs2], 4);
                    fr = fa * fb;
                    memcpy(&lane->fpr[rd], &fr, 4);
                } else if (funct7 == 0x68 && rs2 == 0) {
                    /* FCVT.S.W: int → float */
                    int32_t val_i = (int32_t)lane->gpr[rs1];
                    float val_f = (float)val_i;
                    memcpy(&lane->fpr[rd], &val_f, 4);
                } else if (funct7 == 0x68 && rs2 == 1) {
                    /* FCVT.S.WU: unsigned int → float */
                    float val_f = (float)lane->gpr[rs1];
                    memcpy(&lane->fpr[rd], &val_f, 4);
                } else if (funct7 == 0x60 && rs2 == 0) {
                    /* FCVT.W.S: float → int (RTZ) */
                    float val_f;
                    memcpy(&val_f, &lane->fpr[rs1], 4);
                    if (rd != 0) lane->gpr[rd] = (uint32_t)(int32_t)val_f;
                } else if (funct7 == 0x78) {
                    /* FMV.W.X: int reg → float reg */
                    if (rd != 0) lane->fpr[rd] = lane->gpr[rs1];
                } else if (funct7 == 0x22 && rs2 == 0) {
                    /* FCVT.S.BF16: bf16 → float32 */
                    uint16_t bf16_val = (uint16_t)(lane->fpr[rs1] & 0xFFFF);
                    float result = bf16_to_f32(bf16_val);
                    memcpy(&lane->fpr[rd], &result, 4);
                } else if (funct7 == 0x22 && rs2 == 1) {
                    /* FCVT.BF16.S: float32 → bf16 */
                    float val_f;
                    memcpy(&val_f, &lane->fpr[rs1], 4);
                    uint16_t bf16_val = f32_to_bf16(val_f);
                    lane->fpr[rd] = (lane->fpr[rd] & 0xFFFF0000) | bf16_val;
                } else if (funct7 == 0x24 && rs2 == 1) {
                    /* FCVT.E4M3.S: float32 → e4m3 */
                    float val_f;
                    memcpy(&val_f, &lane->fpr[rs1], 4);
                    uint8_t e4m3_val = f32_to_e4m3(val_f);
                    lane->fpr[rd] = (lane->fpr[rd] & 0xFFFFFF00) | e4m3_val;
                } else if (funct7 == 0x24 && rs2 == 0) {
                    /* FCVT.S.E4M3: e4m3 → float32 */
                    uint8_t e4m3_val = (uint8_t)(lane->fpr[rs1] & 0xFF);
                    float result = e4m3_to_f32(e4m3_val);
                    memcpy(&lane->fpr[rd], &result, 4);
                } else if (funct7 == 0x24 && rs2 == 3) {
                    /* FCVT.E5M2.S: float32 → e5m2 */
                    float val_f;
                    memcpy(&val_f, &lane->fpr[rs1], 4);
                    uint8_t e5m2_val = f32_to_e5m2(val_f);
                    lane->fpr[rd] = (lane->fpr[rd] & 0xFFFFFF00) | e5m2_val;
                } else if (funct7 == 0x24 && rs2 == 2) {
                    /* FCVT.S.E5M2: e5m2 → float32 */
                    uint8_t e5m2_val = (uint8_t)(lane->fpr[rs1] & 0xFF);
                    float result = e5m2_to_f32(e5m2_val);
                    memcpy(&lane->fpr[rd], &result, 4);
                } else if (funct7 == 0x26 && rs2 == 1) {
                    /* FCVT.E2M1.S: float32 → e2m1 */
                    float val_f;
                    memcpy(&val_f, &lane->fpr[rs1], 4);
                    uint8_t e2m1_val = f32_to_e2m1(val_f);
                    lane->fpr[rd] = (lane->fpr[rd] & 0xFFFFFFF0) | e2m1_val;
                } else if (funct7 == 0x26 && rs2 == 0) {
                    /* FCVT.S.E2M1: e2m1 → float32 */
                    uint8_t e2m1_val = (uint8_t)(lane->fpr[rs1] & 0x0F);
                    float result = e2m1_to_f32(e2m1_val);
                    memcpy(&lane->fpr[rd], &result, 4);
                }
                lane->pc += 4;
                break;
            }
            default:
                /* Unknown opcode, skip */
                lane->pc += 4;
                break;
            }

            /* Ensure x0 is always 0 */
            lane->gpr[0] = 0;
        }

        cycle++;
    }

    return 0;
}

/*
 * ============================================================================
 * Kernel dispatch: iterate over grid, execute warps per block
 * ============================================================================
 */
int gpgpu_core_exec_kernel(GPGPUState *s)
{
    uint32_t grid_x = s->kernel.grid_dim[0] ? s->kernel.grid_dim[0] : 1;
    uint32_t grid_y = s->kernel.grid_dim[1] ? s->kernel.grid_dim[1] : 1;
    uint32_t grid_z = s->kernel.grid_dim[2] ? s->kernel.grid_dim[2] : 1;

    uint32_t block_x = s->kernel.block_dim[0] ? s->kernel.block_dim[0] : 1;
    uint32_t block_y = s->kernel.block_dim[1] ? s->kernel.block_dim[1] : 1;
    uint32_t block_z = s->kernel.block_dim[2] ? s->kernel.block_dim[2] : 1;

    uint32_t threads_per_block = block_x * block_y * block_z;

    for (uint32_t bz = 0; bz < grid_z; bz++) {
        for (uint32_t by = 0; by < grid_y; by++) {
            for (uint32_t bx = 0; bx < grid_x; bx++) {
                uint32_t block_id[3] = { bx, by, bz };
                uint32_t block_id_linear = bx + by * grid_x + bz * grid_x * grid_y;

                uint32_t num_warps = (threads_per_block + GPGPU_WARP_SIZE - 1)
                                     / GPGPU_WARP_SIZE;

                for (uint32_t w = 0; w < num_warps; w++) {
                    GPGPUWarp warp;
                    uint32_t thread_id_base = w * GPGPU_WARP_SIZE;
                    uint32_t num_threads = threads_per_block - thread_id_base;
                    if (num_threads > GPGPU_WARP_SIZE) {
                        num_threads = GPGPU_WARP_SIZE;
                    }

                    /* Update SIMT context for this warp */
                    s->simt.block_id[0] = bx;
                    s->simt.block_id[1] = by;
                    s->simt.block_id[2] = bz;
                    s->simt.warp_id = w;

                    gpgpu_core_init_warp(&warp,
                                         (uint32_t)s->kernel.kernel_addr,
                                         thread_id_base, block_id,
                                         num_threads, w, block_id_linear);

                    gpgpu_core_exec_warp(s, &warp, 100000);
                }
            }
        }
    }

    return 0;
}
