/*
 * RISC-V translation routines for Xg233ai custom instructions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* --- Pure TCG implementations --- */

static bool trans_xg233_vadd(DisasContext *ctx, arg_r *a)
{
    TCGv dst_ptr = get_gpr(ctx, a->rd, EXT_NONE);
    TCGv src1_ptr = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv src2_ptr = get_gpr(ctx, a->rs2, EXT_NONE);
    TCGv addr = tcg_temp_new();
    TCGv val1 = tcg_temp_new();
    TCGv val2 = tcg_temp_new();
    TCGv result = tcg_temp_new();

    for (int i = 0; i < 16; i++) {
        tcg_gen_addi_tl(addr, src1_ptr, i * 4);
        tcg_gen_qemu_ld_tl(val1, addr, ctx->mem_idx, MO_TEUL);
        tcg_gen_addi_tl(addr, src2_ptr, i * 4);
        tcg_gen_qemu_ld_tl(val2, addr, ctx->mem_idx, MO_TEUL);
        tcg_gen_add_tl(result, val1, val2);
        tcg_gen_addi_tl(addr, dst_ptr, i * 4);
        tcg_gen_qemu_st_tl(result, addr, ctx->mem_idx, MO_TEUL);
    }
    return true;
}

static bool trans_xg233_crush(DisasContext *ctx, arg_r *a)
{
    TCGv dst_ptr = get_gpr(ctx, a->rd, EXT_NONE);
    TCGv src_ptr = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv n_val = get_gpr(ctx, a->rs2, EXT_NONE);
    TCGv addr = tcg_temp_new();
    TCGv val1 = tcg_temp_new();
    TCGv val2 = tcg_temp_new();
    TCGv packed = tcg_temp_new();
    TCGv n_half = tcg_temp_new();
    TCGv n_odd = tcg_temp_new();

    /* n_half = n / 2 */
    tcg_gen_shri_tl(n_half, n_val, 1);

    /* Loop for i = 0 .. n/2 - 1 */
    TCGLabel *loop = gen_new_label();
    TCGLabel *done = gen_new_label();
    TCGv i_val = tcg_temp_new();
    tcg_gen_movi_tl(i_val, 0);
    gen_set_label(loop);
    tcg_gen_brcond_tl(TCG_COND_GEU, i_val, n_half, done);

    /* src[2*i] — byte address = src_ptr + i*2 */
    tcg_gen_shli_tl(addr, i_val, 1);
    tcg_gen_add_tl(addr, addr, src_ptr);
    tcg_gen_qemu_ld_tl(val1, addr, ctx->mem_idx, MO_UB);

    /* src[2*i + 1] */
    tcg_gen_addi_tl(addr, addr, 1);
    tcg_gen_qemu_ld_tl(val2, addr, ctx->mem_idx, MO_UB);

    /* packed = (val1 & 0x0F) | ((val2 & 0x0F) << 4) */
    tcg_gen_andi_tl(val1, val1, 0x0F);
    tcg_gen_andi_tl(val2, val2, 0x0F);
    tcg_gen_shli_tl(val2, val2, 4);
    tcg_gen_or_tl(packed, val1, val2);

    /* store dst[i] */
    tcg_gen_add_tl(addr, dst_ptr, i_val);
    tcg_gen_qemu_st_tl(packed, addr, ctx->mem_idx, MO_UB);

    tcg_gen_addi_tl(i_val, i_val, 1);
    tcg_gen_br(loop);
    gen_set_label(done);

    /* Handle odd element: if (n & 1) dst[n/2] = src[n-1] & 0x0F */
    TCGLabel *odd_done = gen_new_label();
    tcg_gen_andi_tl(n_odd, n_val, 1);
    tcg_gen_brcondi_tl(TCG_COND_EQ, n_odd, 0, odd_done);

    tcg_gen_addi_tl(addr, src_ptr, 0);
    tcg_gen_add_tl(addr, addr, n_val);
    tcg_gen_addi_tl(addr, addr, -1);
    tcg_gen_qemu_ld_tl(val1, addr, ctx->mem_idx, MO_UB);
    tcg_gen_andi_tl(val1, val1, 0x0F);
    tcg_gen_add_tl(addr, dst_ptr, n_half);
    tcg_gen_qemu_st_tl(val1, addr, ctx->mem_idx, MO_UB);

    gen_set_label(odd_done);
    return true;
}

static bool trans_xg233_expand(DisasContext *ctx, arg_r *a)
{
    TCGv dst_ptr = get_gpr(ctx, a->rd, EXT_NONE);
    TCGv src_ptr = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv n_val = get_gpr(ctx, a->rs2, EXT_NONE);
    TCGv addr = tcg_temp_new();
    TCGv val = tcg_temp_new();
    TCGv lo = tcg_temp_new();
    TCGv hi = tcg_temp_new();

    TCGLabel *loop = gen_new_label();
    TCGLabel *done = gen_new_label();
    TCGv i_val = tcg_temp_new();
    tcg_gen_movi_tl(i_val, 0);
    gen_set_label(loop);
    tcg_gen_brcond_tl(TCG_COND_GEU, i_val, n_val, done);

    /* load src[i] */
    tcg_gen_add_tl(addr, src_ptr, i_val);
    tcg_gen_qemu_ld_tl(val, addr, ctx->mem_idx, MO_UB);

    /* lo = val & 0x0F */
    tcg_gen_andi_tl(lo, val, 0x0F);
    /* hi = (val >> 4) & 0x0F */
    tcg_gen_shri_tl(hi, val, 4);
    tcg_gen_andi_tl(hi, hi, 0x0F);

    /* dst[2*i] = lo */
    tcg_gen_shli_tl(addr, i_val, 1);
    tcg_gen_add_tl(addr, addr, dst_ptr);
    tcg_gen_qemu_st_tl(lo, addr, ctx->mem_idx, MO_UB);

    /* dst[2*i+1] = hi */
    tcg_gen_addi_tl(addr, addr, 1);
    tcg_gen_qemu_st_tl(hi, addr, ctx->mem_idx, MO_UB);

    tcg_gen_addi_tl(i_val, i_val, 1);
    tcg_gen_br(loop);
    gen_set_label(done);

    return true;
}

static bool trans_xg233_vrelu(DisasContext *ctx, arg_r *a)
{
    TCGv dst_ptr = get_gpr(ctx, a->rd, EXT_NONE);
    TCGv src_ptr = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv addr = tcg_temp_new();
    TCGv val = tcg_temp_new();
    TCGv zero = tcg_constant_tl(0);

    for (int i = 0; i < 16; i++) {
        tcg_gen_addi_tl(addr, src_ptr, i * 4);
        tcg_gen_qemu_ld_tl(val, addr, ctx->mem_idx, MO_TESL);
        /* val = (val > 0) ? val : 0 — signed comparison */
        tcg_gen_movcond_tl(TCG_COND_GT, val, val, zero, val, zero);
        tcg_gen_addi_tl(addr, dst_ptr, i * 4);
        tcg_gen_qemu_st_tl(val, addr, ctx->mem_idx, MO_TEUL);
    }
    return true;
}

static bool trans_xg233_vscale(DisasContext *ctx, arg_r *a)
{
    TCGv dst_ptr = get_gpr(ctx, a->rd, EXT_NONE);
    TCGv src_ptr = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv scale = get_gpr(ctx, a->rs2, EXT_NONE);
    TCGv addr = tcg_temp_new();
    TCGv val = tcg_temp_new();
    TCGv result = tcg_temp_new();

    for (int i = 0; i < 16; i++) {
        tcg_gen_addi_tl(addr, src_ptr, i * 4);
        tcg_gen_qemu_ld_tl(val, addr, ctx->mem_idx, MO_TESL);
        /* val is already sign-extended by MO_TESL */
        tcg_gen_mul_tl(result, val, scale);
        tcg_gen_addi_tl(addr, dst_ptr, i * 4);
        tcg_gen_qemu_st_tl(result, addr, ctx->mem_idx, MO_TEUL);
    }
    return true;
}

/* --- Helper-based implementations (system-mode only) --- */

#ifdef CONFIG_USER_ONLY

/* Stubs for linux-user mode: these instructions are not supported */
static bool trans_xg233_sort(DisasContext *ctx, arg_r *a) { return false; }
static bool trans_xg233_dma(DisasContext *ctx, arg_r *a) { return false; }
static bool trans_xg233_gemm(DisasContext *ctx, arg_r *a) { return false; }
static bool trans_xg233_vdot(DisasContext *ctx, arg_r *a) { return false; }
static bool trans_xg233_vmax(DisasContext *ctx, arg_r *a) { return false; }

#else /* !CONFIG_USER_ONLY */

static bool trans_xg233_sort(DisasContext *ctx, arg_r *a)
{
    TCGv arr_ptr = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv k_val = get_gpr(ctx, a->rd, EXT_NONE);
    TCGv n_val = get_gpr(ctx, a->rs2, EXT_NONE);
    gen_helper_xg233_sort(tcg_env, arr_ptr, k_val, n_val);
    return true;
}

static bool trans_xg233_dma(DisasContext *ctx, arg_r *a)
{
    TCGv dst_ptr = get_gpr(ctx, a->rd, EXT_NONE);
    TCGv src_ptr = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv grain = get_gpr(ctx, a->rs2, EXT_NONE);
    gen_helper_xg233_dma(tcg_env, dst_ptr, src_ptr, grain);
    return true;
}

static bool trans_xg233_gemm(DisasContext *ctx, arg_r *a)
{
    TCGv c_ptr = get_gpr(ctx, a->rd, EXT_NONE);
    TCGv a_ptr = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv b_ptr = get_gpr(ctx, a->rs2, EXT_NONE);
    gen_helper_xg233_gemm(tcg_env, c_ptr, a_ptr, b_ptr);
    return true;
}

static bool trans_xg233_vdot(DisasContext *ctx, arg_r *a)
{
    TCGv a_ptr = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv b_ptr = get_gpr(ctx, a->rs2, EXT_NONE);
    TCGv ret = dest_gpr(ctx, a->rd);
    gen_helper_xg233_vdot(ret, tcg_env, a_ptr, b_ptr);
    gen_set_gpr(ctx, a->rd, ret);
    return true;
}

static bool trans_xg233_vmax(DisasContext *ctx, arg_r *a)
{
    TCGv src_ptr = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv n_val = get_gpr(ctx, a->rs2, EXT_NONE);
    TCGv ret = dest_gpr(ctx, a->rd);
    gen_helper_xg233_vmax(ret, tcg_env, src_ptr, n_val);
    gen_set_gpr(ctx, a->rd, ret);
    return true;
}

#endif /* !CONFIG_USER_ONLY */
