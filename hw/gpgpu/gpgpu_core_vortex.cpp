/*
 * QEMU GPGPU - Vortex simx Backend Bridge
 *
 * Copyright (c) 2024-2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * C/C++ bridge between QEMU's GPGPU PCIe frontend (gpgpu.c, pure C)
 * and the Vortex simx cycle-level RISC-V GPGPU simulator (C++).
 *
 * Architecture:
 *   gpgpu.c (C)  →  extern "C" bridge  →  Vortex simx (C++)
 *
 * When CONFIG_GPGPU_VORTEX is not set, the functions in this file
 * return -1 and the original gpgpu_core_exec_kernel() (pure C interpreter)
 * is used instead.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"

/*
 * gpgpu.h uses QOM macros (OBJECT_DECLARE_SIMPLE_TYPE) that differ
 * between C and C++ compilation paths in this QEMU version.
 * To avoid macro expansion conflicts, we include gpgpu.h only
 * when NOT in C++ compilation mode, and use forward declarations
 * for GPGPUState members we need.
 *
 * gpgpu_core.h is safe — it has proper extern "C" guards and
 * only forward-declares GPGPUState.
 */
/*
 * gpgpu_core.h provides:
 *   - GPGPUState forward declaration
 *   - Vortex backend API (extern "C")
 *   - GPGPUState accessor functions (extern "C")
 *
 * gpgpu.h is NOT included because it uses QOM macros
 * (OBJECT_DECLARE_SIMPLE_TYPE) that are incompatible with C++
 * compilation in this QEMU version.
 */
#include "gpgpu_core.h"

#ifdef CONFIG_GPGPU_VORTEX
/*
 * Include Vortex simx C++ headers.
 * Path is relative to the QEMU include path configured in meson.build.
 */
#include "vortex_simx.h"

/* ── Vortex simx state ── */
static void *vx_handle;
static bool  vx_initialized;

/* ── Kernel size tracking ──
 *
 * The GPGPU device doesn't track kernel binary size directly;
 * kernels are uploaded to VRAM via BAR2 writes. The dispatch
 * handler needs the kernel size to load into simx.
 *
 * We track the kernel size from the last DISPATCH write's
 * block_dim + shared_mem, plus the kernel binary header convention:
 * the first 4 bytes of the kernel binary encode its total size.
 */
static uint32_t vortex_kernel_size;

extern "C" {

int gpgpu_core_vortex_init(GPGPUState *s)
{
    if (vx_initialized) {
        return 0;
    }

    VortexSimConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.num_warps    = gpgpu_state_get_warps_per_cu(s) * gpgpu_state_get_num_cus(s);
    cfg.num_threads  = gpgpu_state_get_warp_size(s);
    cfg.num_cores    = gpgpu_state_get_num_cus(s);
    cfg.warp_size    = gpgpu_state_get_warp_size(s);
    cfg.l1_cache_size = 16 * 1024;      /* 16 KB L1 I/D cache */
    cfg.shared_mem_size = 64 * 1024;    /* 64 KB shared memory per core */
    cfg.vram_ptr     = gpgpu_state_get_vram_ptr(s);
    cfg.vram_size    = gpgpu_state_get_vram_size(s);

    vx_handle = vortex_simx_init(&cfg);
    if (!vx_handle) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "GPGPU: vortex_simx_init failed\n");
        return -1;
    }

    qemu_log_mask(LOG_UNIMP,
                  "GPGPU: Vortex simx initialized "
                  "(CUs=%d, warps=%d, threads=%d, VRAM=%ld MB)\n",
                  cfg.num_cores, cfg.num_warps,
                  cfg.num_threads, cfg.vram_size / (1024 * 1024));

    vx_initialized = true;
    return 0;
}

int gpgpu_core_vortex_load_kernel(GPGPUState *s)
{
    uint64_t kaddr = gpgpu_state_get_kernel_addr(s);
    uint64_t vsize = gpgpu_state_get_vram_size(s);
    uint8_t *vram  = gpgpu_state_get_vram_ptr(s);

    if (kaddr + 4 > vsize) {
        return -1;
    }

    /* Read kernel size from binary header (first 4 bytes convention) */
    memcpy(&vortex_kernel_size, vram + kaddr, 4);

    /* Sanity check */
    if (vortex_kernel_size == 0 || vortex_kernel_size > vsize) {
        /* Fallback: estimate from VRAM data after the kernel */
        vortex_kernel_size = 4096;
    }

    if (vortex_simx_load_kernel(vx_handle,
                                vram + kaddr,
                                vortex_kernel_size) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "GPGPU: vortex_simx_load_kernel failed\n");
        return -1;
    }

    return 0;
}

int gpgpu_core_vortex_dispatch(GPGPUState *s)
{
    uint32_t grid_dim[3], block_dim[3];
    uint8_t *vram = gpgpu_state_get_vram_ptr(s);
    uint64_t kargs = gpgpu_state_get_kernel_args(s);
    uint32_t smem  = gpgpu_state_get_shared_mem(s);

    gpgpu_state_get_grid_dim(s, grid_dim);
    gpgpu_state_get_block_dim(s, block_dim);

    if (vortex_simx_dispatch(vx_handle,
                             grid_dim, block_dim,
                             vram + kargs,
                             smem) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "GPGPU: vortex_simx_dispatch failed\n");
        return -1;
    }

    return 0;
}

int gpgpu_core_vortex_run(GPGPUState *s, uint64_t max_cycles)
{
    (void)s;
    vortex_simx_run(vx_handle, max_cycles);
    return 0;
}

bool gpgpu_core_vortex_is_done(GPGPUState *s)
{
    (void)s;
    if (!vx_initialized) {
        return true;
    }
    return vortex_simx_is_done(vx_handle);
}

void gpgpu_core_vortex_reset(GPGPUState *s)
{
    if (!vx_initialized) {
        return;
    }
    vortex_simx_reset(vx_handle);
    vortex_kernel_size = 0;
}

void gpgpu_core_vortex_shutdown(GPGPUState *s)
{
    gpgpu_core_vortex_reset(s);
    if (vx_handle) {
        vortex_simx_free(vx_handle);
        vx_handle = NULL;
        vx_initialized = false;
    }
}

/*
 * Full synchronous kernel execution through Vortex simx.
 *
 * This is the main entry point called from gpgpu.c's REG_DISPATCH handler
 * when use_vortex=true.
 *
 * Returns: 0 on success, -1 on error.
 */
int gpgpu_core_exec_kernel_vortex(GPGPUState *s)
{
    int ret;

    ret = gpgpu_core_vortex_init(s);
    if (ret != 0) {
        return -1;
    }

    ret = gpgpu_core_vortex_load_kernel(s);
    if (ret != 0) {
        return -1;
    }

    ret = gpgpu_core_vortex_dispatch(s);
    if (ret != 0) {
        return -1;
    }

    /* Synchronous execution: run until all warps complete.
     * 0 = no cycle limit (runs until ebreak hits on all warps). */
    gpgpu_core_vortex_run(s, 0);

    gpgpu_core_vortex_reset(s);
    return 0;
}

} /* extern "C" */

#else /* !CONFIG_GPGPU_VORTEX — Vortex not available, provide stubs */

extern "C" {

int gpgpu_core_exec_kernel_vortex(GPGPUState *s)
{
    /* Vortex backend not compiled in. The caller in gpgpu.c
     * should fall back to gpgpu_core_exec_kernel(). */
    (void)s;
    return -1;
}

bool gpgpu_core_vortex_is_done(GPGPUState *s)
{
    (void)s;
    return true;
}

void gpgpu_core_vortex_reset(GPGPUState *s)
{
    (void)s;
}

void gpgpu_core_vortex_shutdown(GPGPUState *s)
{
    (void)s;
}

} /* extern "C" */

#endif /* CONFIG_GPGPU_VORTEX */
