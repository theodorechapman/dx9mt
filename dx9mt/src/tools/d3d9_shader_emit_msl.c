#include "d3d9_shader_emit_msl.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Emit buffer helpers                                                 */
/* ------------------------------------------------------------------ */

typedef struct emit_ctx {
  char *buf;
  uint32_t len;
  uint32_t cap;
  int error;
  const dx9mt_sm_program *prog;
  uint32_t hash;
  int is_vs;
  int major_ver;
  /* Set of c# registers that have def (inline constant) values */
  int def_reg_set[256];
  /* Sampler type per register (from DCL), default 0 = SAMP_2D */
  uint16_t sampler_type_map[16];
  uint8_t input_reg_width[32];
  uint8_t addr_reg_width[32];
} emit_ctx;

#define DX9MT_MSL_NAME_BUFSZ 128
#define DX9MT_MSL_EXPR_BUFSZ 512
#define DX9MT_MSL_RHS_BUFSZ 4096

static void emit(emit_ctx *ctx, const char *fmt, ...) {
  if (ctx->error) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(ctx->buf + ctx->len, ctx->cap - ctx->len, fmt, ap);
  va_end(ap);
  if (n < 0 || (uint32_t)n >= ctx->cap - ctx->len) {
    ctx->error = 1;
  } else {
    ctx->len += (uint32_t)n;
  }
}

/* ------------------------------------------------------------------ */
/* Register name emission                                              */
/* ------------------------------------------------------------------ */

static const dx9mt_sm_dcl_entry *
find_output_dcl(const emit_ctx *ctx, uint16_t reg_number) {
  if (!ctx || !ctx->prog) {
    return NULL;
  }

  for (uint32_t i = 0; i < ctx->prog->dcl_count; ++i) {
    const dx9mt_sm_dcl_entry *d = &ctx->prog->dcls[i];
    if (d->reg_type == DX9MT_SM_REG_OUTPUT && d->reg_number == reg_number) {
      return d;
    }
  }

  return NULL;
}

static void reg_name(char *out, size_t out_sz, const dx9mt_sm_register *r,
                     const emit_ctx *ctx) {
  switch (r->type) {
  case DX9MT_SM_REG_TEMP:
    snprintf(out, out_sz, "r%u", r->number);
    break;
  case DX9MT_SM_REG_INPUT:
    snprintf(out, out_sz, "in.v%u", r->number);
    break;
  case DX9MT_SM_REG_CONST:
    if (r->has_relative) {
      const char *rc = "xyzw";
      snprintf(out, out_sz, "c[clamp(int(a0.%c) + %u, 0, 255)]",
               rc[r->relative_component], r->number);
    } else if (r->number < 256 && ctx->def_reg_set[r->number]) {
      snprintf(out, out_sz, "c_def_%u", r->number);
    } else {
      snprintf(out, out_sz, "c[%u]", r->number);
    }
    break;
  case DX9MT_SM_REG_ADDR:
    /* VS: a0 (address register).  PS: t# (texture coordinate input). */
    if (ctx->is_vs)
      snprintf(out, out_sz, "a%u", r->number);
    else
      snprintf(out, out_sz, "in.t%u", r->number);
    break;
  case DX9MT_SM_REG_RASTOUT:
    if (r->number == 0) snprintf(out, out_sz, "out.position");
    else if (r->number == 1) snprintf(out, out_sz, "out.fog");
    else snprintf(out, out_sz, "out.pointsize");
    break;
  case DX9MT_SM_REG_ATTROUT:
    snprintf(out, out_sz, "out.oD%u", r->number);
    break;
  case DX9MT_SM_REG_OUTPUT:
    /* VS SM<3.0: oT# (texcoord output).  VS SM>=3.0: o# (generic output). */
    if (ctx->is_vs && ctx->major_ver < 3) {
      snprintf(out, out_sz, "out.oT%u", r->number);
    } else if (ctx->is_vs && ctx->major_ver >= 3) {
      const dx9mt_sm_dcl_entry *d = find_output_dcl(ctx, r->number);
      if (d && d->usage == DX9MT_SM_USAGE_POSITION && d->usage_index == 0) {
        snprintf(out, out_sz, "out.position");
      } else {
        snprintf(out, out_sz, "out.o%u", r->number);
      }
    } else {
      snprintf(out, out_sz, "out.o%u", r->number);
    }
    break;
  case DX9MT_SM_REG_COLOROUT:
    snprintf(out, out_sz, "oC%u", r->number);
    break;
  case DX9MT_SM_REG_DEPTHOUT:
    snprintf(out, out_sz, "oDepth");
    break;
  case DX9MT_SM_REG_SAMPLER:
    snprintf(out, out_sz, "s%u", r->number);
    break;
  case DX9MT_SM_REG_CONSTINT:
    snprintf(out, out_sz, "i%u", r->number);
    break;
  case DX9MT_SM_REG_CONSTBOOL:
    snprintf(out, out_sz, "b%u", r->number);
    break;
  case DX9MT_SM_REG_MISCTYPE:
    if (r->number == 0)
      snprintf(out, out_sz, "in.position");
    else
      snprintf(out, out_sz, "(in.front_facing ? 1.0 : -1.0)");
    break;
  default:
    snprintf(out, out_sz, "UNKNOWN%u_%u", r->type, r->number);
    break;
  }
}

/* ------------------------------------------------------------------ */
/* Swizzle and write mask emission                                     */
/* ------------------------------------------------------------------ */

static const char s_comp[] = "xyzw";

static void swizzle_str(char *out, const uint8_t swiz[4]) {
  /* Identity swizzle: emit nothing */
  if (swiz[0] == 0 && swiz[1] == 1 && swiz[2] == 2 && swiz[3] == 3) {
    out[0] = '\0';
    return;
  }
  /* Replicate swizzle (e.g. .xxxx) */
  if (swiz[0] == swiz[1] && swiz[1] == swiz[2] && swiz[2] == swiz[3]) {
    snprintf(out, 8, ".%c", s_comp[swiz[0]]);
    return;
  }
  snprintf(out, 8, ".%c%c%c%c",
           s_comp[swiz[0]], s_comp[swiz[1]],
           s_comp[swiz[2]], s_comp[swiz[3]]);
}

static void wmask_str(char *out, uint8_t mask) {
  if (mask == 0xF) { out[0] = '\0'; return; }
  int p = 0;
  out[p++] = '.';
  if (mask & 1) out[p++] = 'x';
  if (mask & 2) out[p++] = 'y';
  if (mask & 4) out[p++] = 'z';
  if (mask & 8) out[p++] = 'w';
  out[p] = '\0';
}

/* Count number of bits set in mask (for determining result width) */
static int mask_count(uint8_t mask) {
  int c = 0;
  if (mask & 1) c++;
  if (mask & 2) c++;
  if (mask & 4) c++;
  if (mask & 8) c++;
  return c;
}

/* Return "float", "float2", "float3", or "float4" for a given component count */
static const char *float_type_for_count(int count) {
  switch (count) {
  case 1:  return "float";
  case 2:  return "float2";
  case 3:  return "float3";
  default: return "float4";
  }
}

static const char *zero_literal_for_count(int count) {
  switch (count) {
  case 1:  return "0.0";
  case 2:  return "float2(0.0)";
  case 3:  return "float3(0.0)";
  default: return "float4(0.0)";
  }
}

static int dst_width_for_reg(const dx9mt_sm_register *r) {
  int width;

  if (!r) {
    return 4;
  }
  if (r->type == DX9MT_SM_REG_DEPTHOUT) {
    return 1;
  }

  width = mask_count(r->write_mask);
  return width > 0 ? width : 4;
}

static uint8_t reg_available_width(const emit_ctx *ctx,
                                   const dx9mt_sm_register *r) {
  if (!ctx || !r) {
    return 4;
  }

  switch (r->type) {
  case DX9MT_SM_REG_INPUT:
    if (r->number < 32 && ctx->input_reg_width[r->number] != 0)
      return ctx->input_reg_width[r->number];
    break;
  case DX9MT_SM_REG_ADDR:
    if (!ctx->is_vs && r->number < 32 && ctx->addr_reg_width[r->number] != 0)
      return ctx->addr_reg_width[r->number];
    break;
  default:
    break;
  }
  return 4;
}

/* ------------------------------------------------------------------ */
/* Source expression with modifier and swizzle                         */
/* ------------------------------------------------------------------ */

static void reg_component_expr(char *out, size_t out_sz,
                               const dx9mt_sm_register *r,
                               const emit_ctx *ctx, uint8_t component) {
  char base[DX9MT_MSL_NAME_BUFSZ];
  uint8_t available_width;
  reg_name(base, sizeof(base), r, ctx);
  available_width = reg_available_width(ctx, r);

  if (r->type == DX9MT_SM_REG_MISCTYPE && r->number == 1) {
    snprintf(out, out_sz, "%s", base);
  } else if (component >= available_width) {
    snprintf(out, out_sz, "0.0");
  } else {
    snprintf(out, out_sz, "%s.%c", base, s_comp[component & 3u]);
  }
}

static void src_component_expr(char *out, size_t out_sz,
                               const dx9mt_sm_register *r,
                               const emit_ctx *ctx, uint8_t swizzle_index) {
  char comp_expr[DX9MT_MSL_EXPR_BUFSZ];
  char base[DX9MT_MSL_NAME_BUFSZ];

  reg_name(base, sizeof(base), r, ctx);
  reg_component_expr(comp_expr, sizeof(comp_expr), r, ctx,
                     r->swizzle[swizzle_index & 3u]);

  switch (r->src_modifier) {
  case DX9MT_SM_SRCMOD_NONE:
    snprintf(out, out_sz, "%s", comp_expr);
    break;
  case DX9MT_SM_SRCMOD_NEGATE:
    snprintf(out, out_sz, "(-%s)", comp_expr);
    break;
  case DX9MT_SM_SRCMOD_BIAS:
    snprintf(out, out_sz, "(%s - 0.5)", comp_expr);
    break;
  case DX9MT_SM_SRCMOD_BIAS_NEG:
    snprintf(out, out_sz, "(-(%s - 0.5))", comp_expr);
    break;
  case DX9MT_SM_SRCMOD_SIGN:
    snprintf(out, out_sz, "(2.0 * (%s - 0.5))", comp_expr);
    break;
  case DX9MT_SM_SRCMOD_SIGN_NEG:
    snprintf(out, out_sz, "-(2.0 * (%s - 0.5))", comp_expr);
    break;
  case DX9MT_SM_SRCMOD_COMPLEMENT:
    snprintf(out, out_sz, "(1.0 - %s)", comp_expr);
    break;
  case DX9MT_SM_SRCMOD_X2:
    snprintf(out, out_sz, "(%s * 2.0)", comp_expr);
    break;
  case DX9MT_SM_SRCMOD_X2_NEG:
    snprintf(out, out_sz, "(-%s * 2.0)", comp_expr);
    break;
  case DX9MT_SM_SRCMOD_DZ:
    snprintf(out, out_sz, "(%s / %s.z)", comp_expr, base);
    break;
  case DX9MT_SM_SRCMOD_DW:
    snprintf(out, out_sz, "(%s / %s.w)", comp_expr, base);
    break;
  case DX9MT_SM_SRCMOD_ABS:
    snprintf(out, out_sz, "abs(%s)", comp_expr);
    break;
  case DX9MT_SM_SRCMOD_ABS_NEG:
    snprintf(out, out_sz, "(-abs(%s))", comp_expr);
    break;
  case DX9MT_SM_SRCMOD_NOT:
    snprintf(out, out_sz, "(1.0 - %s)", comp_expr);
    break;
  default:
    snprintf(out, out_sz, "%s", comp_expr);
    break;
  }
}

static void src_expr_width(char *out, size_t out_sz, const dx9mt_sm_register *r,
                           const emit_ctx *ctx, int width) {
  char c0[DX9MT_MSL_EXPR_BUFSZ], c1[DX9MT_MSL_EXPR_BUFSZ];
  char c2[DX9MT_MSL_EXPR_BUFSZ], c3[DX9MT_MSL_EXPR_BUFSZ];

  if (width <= 1) {
    src_component_expr(out, out_sz, r, ctx, 0);
    return;
  }

  src_component_expr(c0, sizeof(c0), r, ctx, 0);
  src_component_expr(c1, sizeof(c1), r, ctx, 1);
  if (width == 2) {
    snprintf(out, out_sz, "float2(%s, %s)", c0, c1);
    return;
  }

  src_component_expr(c2, sizeof(c2), r, ctx, 2);
  if (width == 3) {
    snprintf(out, out_sz, "float3(%s, %s, %s)", c0, c1, c2);
    return;
  }

  src_component_expr(c3, sizeof(c3), r, ctx, 3);
  snprintf(out, out_sz, "float4(%s, %s, %s, %s)", c0, c1, c2, c3);
}

static void src_expr(char *out, size_t out_sz, const dx9mt_sm_register *r,
                     const emit_ctx *ctx) {
  src_expr_width(out, out_sz, r, ctx, 4);
}

static const char *comparison_op_str(uint8_t cmp) {
  switch (cmp) {
  case DX9MT_SM_CMP_GT: return ">";
  case DX9MT_SM_CMP_EQ: return "==";
  case DX9MT_SM_CMP_GE: return ">=";
  case DX9MT_SM_CMP_LT: return "<";
  case DX9MT_SM_CMP_NE: return "!=";
  case DX9MT_SM_CMP_LE: return "<=";
  default:              return "!=";
  }
}

static void semantic_user_name(char *out, size_t out_sz,
                               const dx9mt_sm_dcl_entry *d) {
  const char *uname = "attr";

  if (!d) {
    snprintf(out, out_sz, "attr0");
    return;
  }

  if (d->usage == DX9MT_SM_USAGE_TEXCOORD) uname = "texcoord";
  else if (d->usage == DX9MT_SM_USAGE_COLOR) uname = "color";
  else if (d->usage == DX9MT_SM_USAGE_NORMAL) uname = "normal";
  else if (d->usage == DX9MT_SM_USAGE_FOG) uname = "fog";
  else if (d->usage == DX9MT_SM_USAGE_POSITION) uname = "position";
  else if (d->usage == DX9MT_SM_USAGE_TANGENT) uname = "tangent";
  else if (d->usage == DX9MT_SM_USAGE_BINORMAL) uname = "binormal";
  else if (d->usage == DX9MT_SM_USAGE_BLENDWEIGHT) uname = "blendweight";
  else if (d->usage == DX9MT_SM_USAGE_BLENDINDICES) uname = "blendindices";

  snprintf(out, out_sz, "%s%u", uname, d->usage_index);
}

/* ------------------------------------------------------------------ */
/* Instruction emission                                                */
/* ------------------------------------------------------------------ */

static void emit_instruction(emit_ctx *ctx, const dx9mt_sm_instruction *inst) {
  char dst[DX9MT_MSL_NAME_BUFSZ], wm[8];
  char s0[DX9MT_MSL_EXPR_BUFSZ], s1[DX9MT_MSL_EXPR_BUFSZ];
  char s2[DX9MT_MSL_EXPR_BUFSZ];
  char rhs[DX9MT_MSL_RHS_BUFSZ];
  int rhs_is_scalar = 0;
  int dst_width = 4;

  int has_dst = 1;
  switch (inst->opcode) {
  case DX9MT_SM_OP_NOP:
    has_dst = 0;
    break;
  default:
    break;
  }

  if (has_dst) {
    reg_name(dst, sizeof(dst), &inst->dst, ctx);
    wmask_str(wm, inst->dst.write_mask);
    dst_width = dst_width_for_reg(&inst->dst);
  }

  for (int i = 0; i < inst->num_sources && i < 3; ++i) {
    char *tgt = (i == 0) ? s0 : (i == 1) ? s1 : s2;
    src_expr(tgt, 128, &inst->src[i], ctx);
  }

  int do_sat = (has_dst && (inst->dst.result_modifier & DX9MT_SM_RMOD_SATURATE));

  switch (inst->opcode) {
  case DX9MT_SM_OP_NOP:
    return;

  case DX9MT_SM_OP_MOV:
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, dst_width);
    snprintf(rhs, sizeof(rhs), "%s", s0);
    rhs_is_scalar = (dst_width == 1);
    break;

  case DX9MT_SM_OP_ADD:
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, dst_width);
    src_expr_width(s1, sizeof(s1), &inst->src[1], ctx, dst_width);
    snprintf(rhs, sizeof(rhs), "%s + %s", s0, s1);
    rhs_is_scalar = (dst_width == 1);
    break;

  case DX9MT_SM_OP_SUB:
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, dst_width);
    src_expr_width(s1, sizeof(s1), &inst->src[1], ctx, dst_width);
    snprintf(rhs, sizeof(rhs), "%s - %s", s0, s1);
    rhs_is_scalar = (dst_width == 1);
    break;

  case DX9MT_SM_OP_MUL:
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, dst_width);
    src_expr_width(s1, sizeof(s1), &inst->src[1], ctx, dst_width);
    snprintf(rhs, sizeof(rhs), "%s * %s", s0, s1);
    rhs_is_scalar = (dst_width == 1);
    break;

  case DX9MT_SM_OP_MAD:
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, dst_width);
    src_expr_width(s1, sizeof(s1), &inst->src[1], ctx, dst_width);
    src_expr_width(s2, sizeof(s2), &inst->src[2], ctx, dst_width);
    snprintf(rhs, sizeof(rhs), "%s * %s + %s", s0, s1, s2);
    rhs_is_scalar = (dst_width == 1);
    break;

  case DX9MT_SM_OP_DP3:
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, 3);
    src_expr_width(s1, sizeof(s1), &inst->src[1], ctx, 3);
    snprintf(rhs, sizeof(rhs), "dot(%s.xyz, %s.xyz)", s0, s1);
    rhs_is_scalar = 1;
    break;

  case DX9MT_SM_OP_DP4:
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, 4);
    src_expr_width(s1, sizeof(s1), &inst->src[1], ctx, 4);
    snprintf(rhs, sizeof(rhs), "dot(%s, %s)", s0, s1);
    rhs_is_scalar = 1;
    break;

  case DX9MT_SM_OP_RCP:
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, 1);
    snprintf(rhs, sizeof(rhs), "(1.0 / %s)", s0);
    rhs_is_scalar = 1;
    break;

  case DX9MT_SM_OP_RSQ:
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, 1);
    snprintf(rhs, sizeof(rhs), "rsqrt(abs(%s))", s0);
    rhs_is_scalar = 1;
    break;

  case DX9MT_SM_OP_MIN:
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, dst_width);
    src_expr_width(s1, sizeof(s1), &inst->src[1], ctx, dst_width);
    snprintf(rhs, sizeof(rhs), "min(%s, %s)", s0, s1);
    rhs_is_scalar = (dst_width == 1);
    break;

  case DX9MT_SM_OP_MAX:
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, dst_width);
    src_expr_width(s1, sizeof(s1), &inst->src[1], ctx, dst_width);
    snprintf(rhs, sizeof(rhs), "max(%s, %s)", s0, s1);
    rhs_is_scalar = (dst_width == 1);
    break;

  case DX9MT_SM_OP_SLT: {
    int mc = mask_count(inst->dst.write_mask);
    const char *ft = float_type_for_count(mc);
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, mc);
    src_expr_width(s1, sizeof(s1), &inst->src[1], ctx, mc);
    snprintf(rhs, sizeof(rhs), "select(%s(0.0), %s(1.0), (%s < %s))", ft, ft, s0, s1);
    break;
  }

  case DX9MT_SM_OP_SGE: {
    int mc = mask_count(inst->dst.write_mask);
    const char *ft = float_type_for_count(mc);
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, mc);
    src_expr_width(s1, sizeof(s1), &inst->src[1], ctx, mc);
    snprintf(rhs, sizeof(rhs), "select(%s(0.0), %s(1.0), (%s >= %s))", ft, ft, s0, s1);
    break;
  }

  case DX9MT_SM_OP_EXP:
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, 1);
    snprintf(rhs, sizeof(rhs), "exp2(%s)", s0);
    rhs_is_scalar = 1;
    break;

  case DX9MT_SM_OP_LOG:
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, 1);
    snprintf(rhs, sizeof(rhs), "log2(abs(%s))", s0);
    rhs_is_scalar = 1;
    break;

  case DX9MT_SM_OP_FRC:
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, dst_width);
    snprintf(rhs, sizeof(rhs), "fract(%s)", s0);
    rhs_is_scalar = (dst_width == 1);
    break;

  case DX9MT_SM_OP_ABS:
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, dst_width);
    snprintf(rhs, sizeof(rhs), "abs(%s)", s0);
    rhs_is_scalar = (dst_width == 1);
    break;

  case DX9MT_SM_OP_NRM: {
    /* nrm dst, src: normalize xyz, w = 1/length */
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, 3);
    snprintf(rhs, sizeof(rhs),
             "float4(normalize(%s), rsqrt(dot(%s, %s)))", s0, s0, s0);
    break;
  }

  case DX9MT_SM_OP_LRP:
    /* lrp dst, f, a, b = mix(b, a, f) = f*(a-b)+b */
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, dst_width);
    src_expr_width(s1, sizeof(s1), &inst->src[1], ctx, dst_width);
    src_expr_width(s2, sizeof(s2), &inst->src[2], ctx, dst_width);
    snprintf(rhs, sizeof(rhs), "mix(%s, %s, %s)", s2, s1, s0);
    rhs_is_scalar = (dst_width == 1);
    break;

  case DX9MT_SM_OP_CMP: {
    /* cmp dst, src0, src1, src2: per-component (src0 >= 0) ? src1 : src2 */
    int mc = mask_count(inst->dst.write_mask);
    const char *ft = float_type_for_count(mc);
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, mc);
    src_expr_width(s1, sizeof(s1), &inst->src[1], ctx, mc);
    src_expr_width(s2, sizeof(s2), &inst->src[2], ctx, mc);
    snprintf(rhs, sizeof(rhs),
             "select(%s, %s, %s >= %s(0.0))", s2, s1, s0, ft);
    break;
  }

  case DX9MT_SM_OP_POW:
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, 1);
    src_expr_width(s1, sizeof(s1), &inst->src[1], ctx, 1);
    snprintf(rhs, sizeof(rhs), "pow(abs(%s), %s)", s0, s1);
    rhs_is_scalar = 1;
    break;

  case DX9MT_SM_OP_CRS:
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, 3);
    src_expr_width(s1, sizeof(s1), &inst->src[1], ctx, 3);
    snprintf(rhs, sizeof(rhs),
             "float4(cross(%s, %s), 0.0)", s0, s1);
    break;

  case DX9MT_SM_OP_SINCOS: {
    char scalar[DX9MT_MSL_EXPR_BUFSZ];
    src_expr_width(scalar, sizeof(scalar), &inst->src[0], ctx, 1);
    if (inst->dst.write_mask & 1) {
      emit(ctx, "  %s.x = cos(%s);\n", dst, scalar);
    }
    if (inst->dst.write_mask & 2) {
      emit(ctx, "  %s.y = sin(%s);\n", dst, scalar);
    }
    if (inst->dst.write_mask & 4) {
      emit(ctx, "  %s.z = 0.0;\n", dst);
    }
    if (inst->dst.write_mask & 8) {
      emit(ctx, "  %s.w = 0.0;\n", dst);
    }
    return;
  }

  case DX9MT_SM_OP_LIT: {
    /* lit dst, src: standard D3D9 lit computation */
    emit(ctx, "  { // lit\n");
    emit(ctx, "    float4 _ls = %s;\n", s0);
    emit(ctx, "    float _d = max(_ls.x, 0.0);\n");
    emit(ctx, "    float _s = (_ls.x > 0.0) ? pow(max(_ls.y, 0.0), clamp(_ls.w, -128.0, 128.0)) : 0.0;\n");
    snprintf(rhs, sizeof(rhs), "float4(1.0, _d, _s, 1.0)");
    /* We'll close the brace after the assignment */
    if (do_sat)
      emit(ctx, "    %s%s = saturate(%s);\n", dst, wm, rhs);
    else
      emit(ctx, "    %s%s = %s;\n", dst, wm, rhs);
    emit(ctx, "  }\n");
    return;
  }

  case DX9MT_SM_OP_DST:
    /* dst dst, src0, src1: (1, src0.y*src1.y, src0.z, src1.w) */
    src_component_expr(s0, sizeof(s0), &inst->src[0], ctx, 1);
    src_component_expr(s1, sizeof(s1), &inst->src[1], ctx, 1);
    {
      char s0z[DX9MT_MSL_EXPR_BUFSZ], s1w[DX9MT_MSL_EXPR_BUFSZ];
      src_component_expr(s0z, sizeof(s0z), &inst->src[0], ctx, 2);
      src_component_expr(s1w, sizeof(s1w), &inst->src[1], ctx, 3);
      snprintf(rhs, sizeof(rhs),
               "float4(1.0, %s * %s, %s, %s)", s0, s1, s0z, s1w);
    }
    break;

  case DX9MT_SM_OP_DP2ADD:
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, 2);
    src_expr_width(s1, sizeof(s1), &inst->src[1], ctx, 2);
    src_expr_width(s2, sizeof(s2), &inst->src[2], ctx, 1);
    snprintf(rhs, sizeof(rhs),
             "(dot(%s, %s) + %s)", s0, s1, s2);
    rhs_is_scalar = 1;
    break;

  case DX9MT_SM_OP_MOVA:
    /* mova a0, src: integer part of src -> address register */
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx,
                   dst_width == 1 ? 1 : dst_width);
    if (dst_width == 1) {
      snprintf(rhs, sizeof(rhs), "floor(%s + 0.5)", s0);
      rhs_is_scalar = 1;
    } else {
      snprintf(rhs, sizeof(rhs), "floor(%s + %s(0.5))", s0,
               float_type_for_count(dst_width));
    }
    break;

  case DX9MT_SM_OP_M4x4: {
    /* m4x4 dst, src0, src1: dst.x = dp4(src0, c[src1+0]) ... */
    char base_c[DX9MT_MSL_NAME_BUFSZ];
    reg_name(base_c, sizeof(base_c), &inst->src[1], ctx);
    /* src1 is a const register, we need c[n], c[n+1], c[n+2], c[n+3] */
    uint16_t cn = inst->src[1].number;
    emit(ctx, "  { // m4x4\n");
    emit(ctx, "    float4 _mv = %s;\n", s0);
    snprintf(rhs, sizeof(rhs),
             "float4(dot(_mv, c[%u]), dot(_mv, c[%u]), dot(_mv, c[%u]), dot(_mv, c[%u]))",
             cn, cn+1, cn+2, cn+3);
    if (do_sat)
      emit(ctx, "    %s%s = saturate(%s);\n", dst, wm, rhs);
    else
      emit(ctx, "    %s%s = %s;\n", dst, wm, rhs);
    emit(ctx, "  }\n");
    return;
  }

  case DX9MT_SM_OP_M4x3: {
    uint16_t cn = inst->src[1].number;
    emit(ctx, "  { // m4x3\n");
    emit(ctx, "    float4 _mv = %s;\n", s0);
    snprintf(rhs, sizeof(rhs),
             "float4(dot(_mv, c[%u]), dot(_mv, c[%u]), dot(_mv, c[%u]), 1.0)",
             cn, cn+1, cn+2);
    if (do_sat)
      emit(ctx, "    %s%s = saturate(%s);\n", dst, wm, rhs);
    else
      emit(ctx, "    %s%s = %s;\n", dst, wm, rhs);
    emit(ctx, "  }\n");
    return;
  }

  case DX9MT_SM_OP_M3x4: {
    uint16_t cn = inst->src[1].number;
    emit(ctx, "  { // m3x4\n");
    emit(ctx, "    float3 _mv = %s.xyz;\n", s0);
    snprintf(rhs, sizeof(rhs),
             "float4(dot(_mv, c[%u].xyz), dot(_mv, c[%u].xyz), dot(_mv, c[%u].xyz), dot(_mv, c[%u].xyz))",
             cn, cn+1, cn+2, cn+3);
    if (do_sat)
      emit(ctx, "    %s%s = saturate(%s);\n", dst, wm, rhs);
    else
      emit(ctx, "    %s%s = %s;\n", dst, wm, rhs);
    emit(ctx, "  }\n");
    return;
  }

  case DX9MT_SM_OP_M3x3: {
    uint16_t cn = inst->src[1].number;
    emit(ctx, "  { // m3x3\n");
    emit(ctx, "    float3 _mv = %s.xyz;\n", s0);
    snprintf(rhs, sizeof(rhs),
             "float4(dot(_mv, c[%u].xyz), dot(_mv, c[%u].xyz), dot(_mv, c[%u].xyz), 1.0)",
             cn, cn+1, cn+2);
    if (do_sat)
      emit(ctx, "    %s%s = saturate(%s);\n", dst, wm, rhs);
    else
      emit(ctx, "    %s%s = %s;\n", dst, wm, rhs);
    emit(ctx, "  }\n");
    return;
  }

  case DX9MT_SM_OP_M3x2: {
    uint16_t cn = inst->src[1].number;
    emit(ctx, "  { // m3x2\n");
    emit(ctx, "    float3 _mv = %s.xyz;\n", s0);
    snprintf(rhs, sizeof(rhs),
             "float4(dot(_mv, c[%u].xyz), dot(_mv, c[%u].xyz), 0.0, 1.0)",
             cn, cn+1);
    if (do_sat)
      emit(ctx, "    %s%s = saturate(%s);\n", dst, wm, rhs);
    else
      emit(ctx, "    %s%s = %s;\n", dst, wm, rhs);
    emit(ctx, "  }\n");
    return;
  }

  case DX9MT_SM_OP_TEXLD: {
    /* texld dst, coord, sampler */
    uint16_t samp_num = inst->src[1].number;
    char coord_expr[DX9MT_MSL_EXPR_BUFSZ];
    int coord_width = 2;
    if (samp_num < 16 &&
        (ctx->sampler_type_map[samp_num] == DX9MT_SM_SAMP_CUBE ||
         ctx->sampler_type_map[samp_num] == DX9MT_SM_SAMP_VOLUME)) {
      coord_width = 3;
    }
    src_expr_width(coord_expr, sizeof(coord_expr), &inst->src[0], ctx,
                   coord_width);
    snprintf(rhs, sizeof(rhs), "tex%u.sample(samp%u, %s)",
             samp_num, samp_num, coord_expr);
    break;
  }

  case DX9MT_SM_OP_TEXLDL: {
    /* texldl dst, coord, sampler: coord.w = LOD */
    uint16_t samp_num = inst->src[1].number;
    char coord_expr[DX9MT_MSL_EXPR_BUFSZ];
    char lod_expr[DX9MT_MSL_EXPR_BUFSZ];
    int coord_width = 2;
    if (samp_num < 16 &&
        (ctx->sampler_type_map[samp_num] == DX9MT_SM_SAMP_CUBE ||
         ctx->sampler_type_map[samp_num] == DX9MT_SM_SAMP_VOLUME)) {
      coord_width = 3;
    }
    src_expr_width(coord_expr, sizeof(coord_expr), &inst->src[0], ctx,
                   coord_width);
    src_component_expr(lod_expr, sizeof(lod_expr), &inst->src[0], ctx, 3);
    snprintf(rhs, sizeof(rhs), "tex%u.sample(samp%u, %s, level(%s))",
             samp_num, samp_num, coord_expr, lod_expr);
    break;
  }

  case DX9MT_SM_OP_TEXKILL:
    emit(ctx, "  if (any(%s.xyz < float3(0.0))) discard_fragment();\n", dst);
    return;

  case DX9MT_SM_OP_IFC: {
    char s0e[DX9MT_MSL_EXPR_BUFSZ], s1e[DX9MT_MSL_EXPR_BUFSZ];
    src_expr_width(s0e, sizeof(s0e), &inst->src[0], ctx, 1);
    src_expr_width(s1e, sizeof(s1e), &inst->src[1], ctx, 1);
    emit(ctx, "  if (%s %s %s) {\n",
         s0e, comparison_op_str(inst->comparison), s1e);
    return;
  }

  case DX9MT_SM_OP_IF: {
    char s0e[DX9MT_MSL_EXPR_BUFSZ];
    src_expr_width(s0e, sizeof(s0e), &inst->src[0], ctx, 1);
    emit(ctx, "  if (%s != 0.0) {\n", s0e);
    return;
  }

  case DX9MT_SM_OP_ELSE:
    emit(ctx, "  } else {\n");
    return;

  case DX9MT_SM_OP_ENDIF:
    emit(ctx, "  }\n");
    return;

  case DX9MT_SM_OP_REP: {
    char s0e[DX9MT_MSL_EXPR_BUFSZ];
    src_expr_width(s0e, sizeof(s0e), &inst->src[0], ctx, 1);
    emit(ctx, "  for (int rep_i = 0; rep_i < int(%s); rep_i++) {\n", s0e);
    return;
  }

  case DX9MT_SM_OP_ENDREP:
    emit(ctx, "  }\n");
    return;

  case DX9MT_SM_OP_BREAK:
    emit(ctx, "  break;\n");
    return;

  case DX9MT_SM_OP_BREAKC: {
    char s0e[DX9MT_MSL_EXPR_BUFSZ], s1e[DX9MT_MSL_EXPR_BUFSZ];
    src_expr_width(s0e, sizeof(s0e), &inst->src[0], ctx, 1);
    src_expr_width(s1e, sizeof(s1e), &inst->src[1], ctx, 1);
    emit(ctx, "  if (%s %s %s) break;\n",
         s0e, comparison_op_str(inst->comparison), s1e);
    return;
  }

  case DX9MT_SM_OP_SGN:
    /* sgn dst, src0, src1(scratch), src2(scratch) -- only src0 matters */
    src_expr_width(s0, sizeof(s0), &inst->src[0], ctx, dst_width);
    snprintf(rhs, sizeof(rhs), "sign(%s)", s0);
    rhs_is_scalar = (dst_width == 1);
    break;

  default:
    emit(ctx, "  // unsupported opcode %u\n", inst->opcode);
    return;
  }

  /* Broadcast scalar RHS to match destination width when needed.
   * Single-component write mask: scalar is fine.
   * Multi-component: need floatN() wrapper so types match. */
  char final_rhs[DX9MT_MSL_RHS_BUFSZ];
  if (rhs_is_scalar && has_dst) {
    int mc = dst_width;
    if (mc == 1)
      snprintf(final_rhs, sizeof(final_rhs), "%s", rhs);
    else if (mc == 2)
      snprintf(final_rhs, sizeof(final_rhs), "float2(%s)", rhs);
    else if (mc == 3)
      snprintf(final_rhs, sizeof(final_rhs), "float3(%s)", rhs);
    else
      snprintf(final_rhs, sizeof(final_rhs), "float4(%s)", rhs);
  } else {
    snprintf(final_rhs, sizeof(final_rhs), "%s", rhs);
  }

  /* Truncate vector RHS to match write-mask width when needed.
   * E.g. dst.xy = float4_val → dst.xy = (float4_val).xy
   * Scalar RHS (from replicate swizzle) broadcasts naturally in MSL. */
  if (!rhs_is_scalar && has_dst && dst_width < 4) {
    int mc = dst_width;
    int rhs_width = 4;
    if (inst->num_sources == 1) {
      const uint8_t *sw = inst->src[0].swizzle;
      if (sw[0] == sw[1] && sw[1] == sw[2] && sw[2] == sw[3])
        rhs_width = 1;
    } else if (inst->num_sources >= 2) {
      int all_scalar = 1;
      for (int i = 0; i < inst->num_sources && i < 3; ++i) {
        const uint8_t *sw = inst->src[i].swizzle;
        if (!(sw[0] == sw[1] && sw[1] == sw[2] && sw[2] == sw[3])) {
          all_scalar = 0;
          break;
        }
      }
      if (all_scalar) rhs_width = 1;
    }
    if (mc < rhs_width) {
      static const char *trunc_swiz[] = {"", ".x", ".xy", ".xyz", ""};
      char tmp[DX9MT_MSL_RHS_BUFSZ];
      snprintf(tmp, sizeof(tmp), "(%s)%s", final_rhs, trunc_swiz[mc]);
      snprintf(final_rhs, sizeof(final_rhs), "%s", tmp);
    }
  }

  /* Standard assignment with optional saturation */
  if (do_sat)
    emit(ctx, "  %s%s = saturate(%s);\n", dst, wm, final_rhs);
  else
    emit(ctx, "  %s%s = %s;\n", dst, wm, final_rhs);
}

/* ------------------------------------------------------------------ */
/* Semantic-to-attribute-index mapping (must match create_translated_pso) */
/* ------------------------------------------------------------------ */

static int usage_to_attr_idx(uint8_t usage, uint8_t usage_index) {
  if ((usage == DX9MT_SM_USAGE_POSITION || usage == DX9MT_SM_USAGE_POSITIONT)
      && usage_index == 0) return 0;
  if (usage == DX9MT_SM_USAGE_COLOR && usage_index == 0) return 1;
  if (usage == DX9MT_SM_USAGE_TEXCOORD && usage_index == 0) return 2;
  if (usage == DX9MT_SM_USAGE_NORMAL && usage_index == 0) return 3;
  if (usage == DX9MT_SM_USAGE_TANGENT && usage_index == 0) return 4;
  if (usage == DX9MT_SM_USAGE_BINORMAL && usage_index == 0) return 5;
  if (usage == DX9MT_SM_USAGE_BLENDWEIGHT && usage_index == 0) return 6;
  if (usage == DX9MT_SM_USAGE_BLENDINDICES && usage_index == 0) return 7;
  if (usage == DX9MT_SM_USAGE_TEXCOORD && usage_index == 1) return 8;
  if (usage == DX9MT_SM_USAGE_COLOR && usage_index == 1) return 9;
  if (usage == DX9MT_SM_USAGE_TEXCOORD && usage_index == 2) return 10;
  if (usage == DX9MT_SM_USAGE_TEXCOORD && usage_index == 3) return 11;
  if (usage == DX9MT_SM_USAGE_TEXCOORD && usage_index == 4) return 12;
  if (usage == DX9MT_SM_USAGE_TEXCOORD && usage_index == 5) return 13;
  return -1; /* unmapped */
}

/* ------------------------------------------------------------------ */
/* VS emitter                                                          */
/* ------------------------------------------------------------------ */

int dx9mt_msl_emit_vs(const dx9mt_sm_program *prog, uint32_t bytecode_hash,
                      dx9mt_msl_emit_result *out) {
  memset(out, 0, sizeof(*out));
  snprintf(out->entry_name, sizeof(out->entry_name), "vs_%08x", bytecode_hash);

  if (prog->shader_type != 1) {
    snprintf(out->error_msg, sizeof(out->error_msg), "not a vertex shader");
    out->has_error = 1;
    return -1;
  }

  emit_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.buf = out->source;
  ctx.cap = DX9MT_MSL_MAX_SOURCE;
  ctx.prog = prog;
  ctx.hash = bytecode_hash;
  ctx.is_vs = 1;
  ctx.major_ver = prog->major_version;

  /* Mark which c# registers have def values */
  for (uint32_t i = 0; i < prog->def_count; ++i) {
    if (prog->defs[i].reg_type == DX9MT_SM_REG_CONST &&
        prog->defs[i].reg_number < 256) {
      ctx.def_reg_set[prog->defs[i].reg_number] = 1;
    }
  }

  for (uint32_t i = 0; i < prog->dcl_count; ++i) {
    const dx9mt_sm_dcl_entry *d = &prog->dcls[i];
    uint8_t width = (uint8_t)mask_count(d->write_mask);
    if (width == 0) width = 4;
    if (d->reg_type == DX9MT_SM_REG_INPUT && d->reg_number < 32) {
      ctx.input_reg_width[d->reg_number] = width;
    } else if (d->reg_type == DX9MT_SM_REG_ADDR && d->reg_number < 32) {
      ctx.addr_reg_width[d->reg_number] = width;
    }
  }

  emit(&ctx, "#include <metal_stdlib>\n");
  emit(&ctx, "using namespace metal;\n\n");

  /* Input struct from vertex attributes.
   * Variable name = v{reg_number} (matches shader instructions).
   * Attribute index = usage_to_attr_idx (matches PSO vertex descriptor). */
  emit(&ctx, "struct VS_In_%08x {\n", bytecode_hash);
  for (uint32_t i = 0; i < prog->dcl_count; ++i) {
    const dx9mt_sm_dcl_entry *d = &prog->dcls[i];
    if (d->reg_type != DX9MT_SM_REG_INPUT) continue;

    int attr_idx = usage_to_attr_idx(d->usage, d->usage_index);
    if (attr_idx < 0) continue; /* skip unmapped semantics */

    const char *type = "float4";
    int mc = mask_count(d->write_mask);
    if (mc == 1) type = "float";
    else if (mc == 2) type = "float2";
    else if (mc == 3) type = "float3";

    emit(&ctx, "  %s v%u [[attribute(%u)]];\n", type, d->reg_number, attr_idx);
  }
  emit(&ctx, "};\n\n");

  /* Output struct (interpolants to PS) */
  emit(&ctx, "struct VS_Out_%08x {\n", bytecode_hash);
  emit(&ctx, "  float4 position [[position]];\n");

  /* VS 3.0: output registers from dcl with [[user(...)]] for rasterizer matching */
  if (prog->major_version >= 3) {
    for (uint32_t i = 0; i < prog->dcl_count; ++i) {
      const dx9mt_sm_dcl_entry *d = &prog->dcls[i];
      if (d->reg_type != DX9MT_SM_REG_OUTPUT) continue;
      /* Skip position (already emitted above) */
      if (d->usage == DX9MT_SM_USAGE_POSITION && d->usage_index == 0) continue;
      const char *type = "float4";

      {
        char uname[64];
        semantic_user_name(uname, sizeof(uname), d);
        emit(&ctx, "  %s o%u [[user(%s)]];\n", type, d->reg_number, uname);
      }
    }
  } else {
    /* VS 1.x/2.x: oD# (color) and oT# (texcoord) outputs */
    for (int i = 0; i < 2; ++i) {
      if (prog->color_output_mask & (1u << i))
        emit(&ctx, "  float4 oD%d [[user(color%d)]];\n", i, i);
    }
    /* oT# from output_mask (type 6 = TEXCRDOUT in SM < 3.0) */
    for (int i = 0; i < 8; ++i) {
      if (prog->output_mask & (1u << i))
        emit(&ctx, "  float4 oT%d [[user(texcoord%d)]];\n", i, i);
    }
  }

  if (prog->writes_fog)
    emit(&ctx, "  float fog;\n");
  emit(&ctx, "};\n\n");

  /* Vertex function */
  emit(&ctx, "vertex VS_Out_%08x %s(\n", bytecode_hash, out->entry_name);
  emit(&ctx, "    VS_In_%08x in [[stage_in]],\n", bytecode_hash);
  emit(&ctx, "    constant float4 *c [[buffer(1)]]) {\n");

  /* Declare temp registers */
  for (uint32_t i = 0; i <= prog->max_temp_reg; ++i) {
    emit(&ctx, "  float4 r%u = float4(0.0);\n", i);
  }

  /* Declare address register if used */
  for (uint32_t i = 0; i < prog->instruction_count; ++i) {
    if (prog->instructions[i].dst.type == DX9MT_SM_REG_ADDR ||
        prog->instructions[i].opcode == DX9MT_SM_OP_MOVA) {
      emit(&ctx, "  float4 a0 = float4(0.0);\n");
      break;
    }
  }

  /* Inline def constants */
  for (uint32_t i = 0; i < prog->def_count; ++i) {
    const dx9mt_sm_def_entry *d = &prog->defs[i];
    if (d->reg_type == DX9MT_SM_REG_CONST) {
      emit(&ctx, "  float4 c_def_%u = float4(%.9g, %.9g, %.9g, %.9g);\n",
           d->reg_number, d->values.f[0], d->values.f[1],
           d->values.f[2], d->values.f[3]);
    } else if (d->reg_type == DX9MT_SM_REG_CONSTINT) {
      emit(&ctx, "  float4 i%u = float4(%d.0, %d.0, %d.0, %d.0);\n",
           d->reg_number, d->values.i[0], d->values.i[1],
           d->values.i[2], d->values.i[3]);
    } else if (d->reg_type == DX9MT_SM_REG_CONSTBOOL) {
      emit(&ctx, "  float4 b%u = float4(%s, 0.0, 0.0, 0.0);\n",
           d->reg_number, d->values.b ? "1.0" : "0.0");
    }
  }

  for (uint32_t i = 0; i < prog->dcl_count; ++i) {
    const dx9mt_sm_dcl_entry *d = &prog->dcls[i];
    uint8_t width = (uint8_t)mask_count(d->write_mask);
    if (width == 0) width = 4;
    if (d->reg_type == DX9MT_SM_REG_INPUT && d->reg_number < 32) {
      ctx.input_reg_width[d->reg_number] = width;
    } else if (d->reg_type == DX9MT_SM_REG_ADDR && d->reg_number < 32) {
      ctx.addr_reg_width[d->reg_number] = width;
    }
  }

  emit(&ctx, "  VS_Out_%08x out;\n", bytecode_hash);
  emit(&ctx, "  out.position = float4(0.0);\n");
  if (prog->major_version >= 3) {
    for (uint32_t i = 0; i < prog->dcl_count; ++i) {
      const dx9mt_sm_dcl_entry *d = &prog->dcls[i];
      if (d->reg_type != DX9MT_SM_REG_OUTPUT) continue;
      if (d->usage == DX9MT_SM_USAGE_POSITION && d->usage_index == 0) continue;
      emit(&ctx, "  out.o%u = float4(0.0);\n", d->reg_number);
    }
  } else {
    for (int i = 0; i < 2; ++i) {
      if (prog->color_output_mask & (1u << i))
        emit(&ctx, "  out.oD%d = float4(0.0);\n", i);
    }
    for (int i = 0; i < 8; ++i) {
      if (prog->output_mask & (1u << i))
        emit(&ctx, "  out.oT%d = float4(0.0);\n", i);
    }
  }
  emit(&ctx, "\n");

  /* Emit instructions */
  for (uint32_t i = 0; i < prog->instruction_count; ++i) {
    emit_instruction(&ctx, &prog->instructions[i]);
  }

  emit(&ctx, "\n  return out;\n");
  emit(&ctx, "}\n");

  if (ctx.error) {
    snprintf(out->error_msg, sizeof(out->error_msg), "MSL source buffer overflow");
    out->has_error = 1;
    return -1;
  }

  out->source_len = ctx.len;
  return 0;
}

/* ------------------------------------------------------------------ */
/* PS emitter                                                          */
/* ------------------------------------------------------------------ */

int dx9mt_msl_emit_ps(const dx9mt_sm_program *prog, uint32_t bytecode_hash,
                      dx9mt_msl_emit_result *out) {
  memset(out, 0, sizeof(*out));
  snprintf(out->entry_name, sizeof(out->entry_name), "ps_%08x", bytecode_hash);

  if (prog->shader_type != 0) {
    snprintf(out->error_msg, sizeof(out->error_msg), "not a pixel shader");
    out->has_error = 1;
    return -1;
  }

  emit_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.buf = out->source;
  ctx.cap = DX9MT_MSL_MAX_SOURCE;
  ctx.prog = prog;
  ctx.hash = bytecode_hash;
  ctx.is_vs = 0;
  ctx.major_ver = prog->major_version;

  for (uint32_t i = 0; i < prog->def_count; ++i) {
    if (prog->defs[i].reg_type == DX9MT_SM_REG_CONST &&
        prog->defs[i].reg_number < 256) {
      ctx.def_reg_set[prog->defs[i].reg_number] = 1;
    }
  }

  /* Build sampler type map from DCL entries */
  for (uint32_t i = 0; i < prog->dcl_count; ++i) {
    const dx9mt_sm_dcl_entry *d = &prog->dcls[i];
    if (d->reg_type == DX9MT_SM_REG_SAMPLER && d->reg_number < 16) {
      ctx.sampler_type_map[d->reg_number] = d->sampler_type;
    }
  }

  emit(&ctx, "#include <metal_stdlib>\n");
  emit(&ctx, "using namespace metal;\n\n");

  /* Input struct (interpolants from VS) */
  emit(&ctx, "struct PS_In_%08x {\n", bytecode_hash);
  emit(&ctx, "  float4 position [[position]];\n");

  /* PS inputs. For PS 1.x/2.x, DCL semantics on inputs are not reliable;
   * link by fixed-function register role instead:
   *   v# -> color#
   *   t# -> texcoord#
   */
  for (uint32_t i = 0; i < prog->dcl_count; ++i) {
    const dx9mt_sm_dcl_entry *d = &prog->dcls[i];
    if (prog->major_version < 3) {
      if (d->reg_type == DX9MT_SM_REG_INPUT) {
        emit(&ctx, "  float4 v%u [[user(color%u)]];\n", d->reg_number,
             d->reg_number);
      } else if (d->reg_type == DX9MT_SM_REG_ADDR) {
        emit(&ctx, "  float4 t%u [[user(texcoord%u)]];\n", d->reg_number,
             d->reg_number);
      }
    } else if (d->reg_type == DX9MT_SM_REG_INPUT) {
      const char *type = "float4";
      char uname[64];
      semantic_user_name(uname, sizeof(uname), d);
      emit(&ctx, "  %s v%u [[user(%s)]];\n", type, d->reg_number, uname);
    }
  }

  /* vPos/vFace if used */
  for (uint32_t i = 0; i < prog->dcl_count; ++i) {
    if (prog->dcls[i].reg_type == DX9MT_SM_REG_MISCTYPE) {
      if (prog->dcls[i].reg_number == 0) {
        emit(&ctx, "  // vPos mapped to position\n");
      } else if (prog->dcls[i].reg_number == 1) {
        emit(&ctx, "  bool front_facing [[front_facing]];\n");
      }
    }
  }
  emit(&ctx, "};\n\n");

  int needs_output_struct = (prog->num_color_outputs > 1 || prog->writes_depth);

  /* PS output struct for MRT or depth output */
  if (needs_output_struct) {
    emit(&ctx, "struct PS_Out_%08x {\n", bytecode_hash);
    for (int i = 0; i < prog->num_color_outputs; ++i) {
      emit(&ctx, "  float4 color%d [[color(%d)]];\n", i, i);
    }
    if (prog->writes_depth) {
      emit(&ctx, "  float depth [[depth(any)]];\n");
    }
    emit(&ctx, "};\n\n");
  }

  /* Fragment function */
  if (needs_output_struct)
    emit(&ctx, "fragment PS_Out_%08x %s(\n", bytecode_hash, out->entry_name);
  else
    emit(&ctx, "fragment float4 %s(\n", out->entry_name);
  emit(&ctx, "    PS_In_%08x in [[stage_in]]", bytecode_hash);

  /* Texture and sampler arguments */
  for (uint32_t i = 0; i < prog->dcl_count; ++i) {
    const dx9mt_sm_dcl_entry *d = &prog->dcls[i];
    if (d->reg_type != DX9MT_SM_REG_SAMPLER) continue;

    const char *tex_type = "texture2d<float>";
    if (d->sampler_type == DX9MT_SM_SAMP_CUBE)
      tex_type = "texturecube<float>";
    else if (d->sampler_type == DX9MT_SM_SAMP_VOLUME)
      tex_type = "texture3d<float>";

    emit(&ctx, ",\n    %s tex%u [[texture(%u)]]", tex_type, d->reg_number, d->reg_number);
    emit(&ctx, ",\n    sampler samp%u [[sampler(%u)]]", d->reg_number, d->reg_number);
  }

  emit(&ctx, ",\n    constant float4 *c [[buffer(0)]]) {\n");

  /* Declare temp registers */
  for (uint32_t i = 0; i <= prog->max_temp_reg; ++i) {
    emit(&ctx, "  float4 r%u = float4(0.0);\n", i);
  }

  /* Output color registers */
  for (int i = 0; i < prog->num_color_outputs || i < 1; ++i) {
    emit(&ctx, "  float4 oC%d = float4(0.0);\n", i);
  }
  if (prog->writes_depth) {
    emit(&ctx, "  float oDepth = 0.0;\n");
  }

  /* Inline def constants */
  for (uint32_t i = 0; i < prog->def_count; ++i) {
    const dx9mt_sm_def_entry *d = &prog->defs[i];
    if (d->reg_type == DX9MT_SM_REG_CONST) {
      emit(&ctx, "  float4 c_def_%u = float4(%.9g, %.9g, %.9g, %.9g);\n",
           d->reg_number, d->values.f[0], d->values.f[1],
           d->values.f[2], d->values.f[3]);
    } else if (d->reg_type == DX9MT_SM_REG_CONSTINT) {
      emit(&ctx, "  float4 i%u = float4(%d.0, %d.0, %d.0, %d.0);\n",
           d->reg_number, d->values.i[0], d->values.i[1],
           d->values.i[2], d->values.i[3]);
    } else if (d->reg_type == DX9MT_SM_REG_CONSTBOOL) {
      emit(&ctx, "  float4 b%u = float4(%s, 0.0, 0.0, 0.0);\n",
           d->reg_number, d->values.b ? "1.0" : "0.0");
    }
  }

  emit(&ctx, "\n");

  /* Emit instructions */
  for (uint32_t i = 0; i < prog->instruction_count; ++i) {
    emit_instruction(&ctx, &prog->instructions[i]);
  }

  /* Return */
  if (needs_output_struct) {
    emit(&ctx, "\n  PS_Out_%08x ps_out;\n", bytecode_hash);
    for (int i = 0; i < prog->num_color_outputs; ++i) {
      emit(&ctx, "  ps_out.color%d = oC%d;\n", i, i);
    }
    if (prog->writes_depth) {
      emit(&ctx, "  ps_out.depth = oDepth;\n");
    }
    emit(&ctx, "  return ps_out;\n");
  } else {
    emit(&ctx, "\n  return oC0;\n");
  }
  emit(&ctx, "}\n");

  if (ctx.error) {
    snprintf(out->error_msg, sizeof(out->error_msg), "MSL source buffer overflow");
    out->has_error = 1;
    return -1;
  }

  out->source_len = ctx.len;
  return 0;
}
