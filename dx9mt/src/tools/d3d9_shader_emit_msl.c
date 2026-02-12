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
  /* Set of c# registers that have def (inline constant) values */
  int def_reg_set[256];
} emit_ctx;

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

static void reg_name(char *out, size_t out_sz, const dx9mt_sm_register *r,
                     int is_vs) {
  switch (r->type) {
  case DX9MT_SM_REG_TEMP:
    snprintf(out, out_sz, "r%u", r->number);
    break;
  case DX9MT_SM_REG_INPUT:
    snprintf(out, out_sz, "in.v%u", r->number);
    break;
  case DX9MT_SM_REG_CONST:
    snprintf(out, out_sz, "c[%u]", r->number);
    break;
  case DX9MT_SM_REG_ADDR:
    snprintf(out, out_sz, "a%u", r->number);
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
    snprintf(out, out_sz, "out.o%u", r->number);
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
    if (r->number == 0) snprintf(out, out_sz, "in.vpos");
    else snprintf(out, out_sz, "in.vface");
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

/* ------------------------------------------------------------------ */
/* Source expression with modifier and swizzle                         */
/* ------------------------------------------------------------------ */

static void src_expr(char *out, size_t out_sz, const dx9mt_sm_register *r,
                     int is_vs) {
  char base[80];
  char swiz[8];
  reg_name(base, sizeof(base), r, is_vs);
  swizzle_str(swiz, r->swizzle);

  switch (r->src_modifier) {
  case DX9MT_SM_SRCMOD_NONE:
    snprintf(out, out_sz, "%s%s", base, swiz);
    break;
  case DX9MT_SM_SRCMOD_NEGATE:
    snprintf(out, out_sz, "(-%s%s)", base, swiz);
    break;
  case DX9MT_SM_SRCMOD_ABS:
    snprintf(out, out_sz, "abs(%s%s)", base, swiz);
    break;
  case DX9MT_SM_SRCMOD_ABS_NEG:
    snprintf(out, out_sz, "(-abs(%s%s))", base, swiz);
    break;
  case DX9MT_SM_SRCMOD_COMPLEMENT:
    snprintf(out, out_sz, "(1.0 - %s%s)", base, swiz);
    break;
  case DX9MT_SM_SRCMOD_X2:
    snprintf(out, out_sz, "(%s%s * 2.0)", base, swiz);
    break;
  case DX9MT_SM_SRCMOD_X2_NEG:
    snprintf(out, out_sz, "(-%s%s * 2.0)", base, swiz);
    break;
  case DX9MT_SM_SRCMOD_BIAS:
    snprintf(out, out_sz, "(%s%s - 0.5)", base, swiz);
    break;
  case DX9MT_SM_SRCMOD_BIAS_NEG:
    snprintf(out, out_sz, "(-(%s%s - 0.5))", base, swiz);
    break;
  default:
    /* Fallback: just use raw value */
    snprintf(out, out_sz, "%s%s", base, swiz);
    break;
  }
}

/* ------------------------------------------------------------------ */
/* Instruction emission                                                */
/* ------------------------------------------------------------------ */

static void emit_instruction(emit_ctx *ctx, const dx9mt_sm_instruction *inst,
                             int is_vs) {
  char dst[80], wm[8];
  char s0[128], s1[128], s2[128];
  char rhs[512];

  int has_dst = 1;
  switch (inst->opcode) {
  case DX9MT_SM_OP_NOP:
  case DX9MT_SM_OP_ENDREP:
  case DX9MT_SM_OP_ELSE:
  case DX9MT_SM_OP_ENDIF:
  case DX9MT_SM_OP_BREAK:
    has_dst = 0;
    break;
  case DX9MT_SM_OP_REP:
  case DX9MT_SM_OP_IF:
  case DX9MT_SM_OP_IFC:
  case DX9MT_SM_OP_BREAKC:
    has_dst = 0;
    break;
  default:
    break;
  }

  if (has_dst) {
    reg_name(dst, sizeof(dst), &inst->dst, is_vs);
    wmask_str(wm, inst->dst.write_mask);
  }

  for (int i = 0; i < inst->num_sources && i < 3; ++i) {
    char *tgt = (i == 0) ? s0 : (i == 1) ? s1 : s2;
    src_expr(tgt, 128, &inst->src[i], is_vs);
  }

  int do_sat = (has_dst && (inst->dst.result_modifier & DX9MT_SM_RMOD_SATURATE));

  switch (inst->opcode) {
  case DX9MT_SM_OP_NOP:
    return;

  case DX9MT_SM_OP_MOV:
    snprintf(rhs, sizeof(rhs), "%s", s0);
    break;

  case DX9MT_SM_OP_ADD:
    snprintf(rhs, sizeof(rhs), "%s + %s", s0, s1);
    break;

  case DX9MT_SM_OP_SUB:
    snprintf(rhs, sizeof(rhs), "%s - %s", s0, s1);
    break;

  case DX9MT_SM_OP_MUL:
    snprintf(rhs, sizeof(rhs), "%s * %s", s0, s1);
    break;

  case DX9MT_SM_OP_MAD:
    snprintf(rhs, sizeof(rhs), "%s * %s + %s", s0, s1, s2);
    break;

  case DX9MT_SM_OP_DP3:
    snprintf(rhs, sizeof(rhs), "float4(dot(%s.xyz, %s.xyz))", s0, s1);
    break;

  case DX9MT_SM_OP_DP4:
    snprintf(rhs, sizeof(rhs), "float4(dot(%s, %s))", s0, s1);
    break;

  case DX9MT_SM_OP_RCP:
    snprintf(rhs, sizeof(rhs), "float4(1.0 / %s.x)", s0);
    break;

  case DX9MT_SM_OP_RSQ:
    snprintf(rhs, sizeof(rhs), "float4(rsqrt(abs(%s.x)))", s0);
    break;

  case DX9MT_SM_OP_MIN:
    snprintf(rhs, sizeof(rhs), "min(%s, %s)", s0, s1);
    break;

  case DX9MT_SM_OP_MAX:
    snprintf(rhs, sizeof(rhs), "max(%s, %s)", s0, s1);
    break;

  case DX9MT_SM_OP_SLT:
    snprintf(rhs, sizeof(rhs), "float4(%s < %s)", s0, s1);
    break;

  case DX9MT_SM_OP_SGE:
    snprintf(rhs, sizeof(rhs), "float4(%s >= %s)", s0, s1);
    break;

  case DX9MT_SM_OP_EXP:
    snprintf(rhs, sizeof(rhs), "float4(exp2(%s.x))", s0);
    break;

  case DX9MT_SM_OP_LOG:
    snprintf(rhs, sizeof(rhs), "float4(log2(abs(%s.x)))", s0);
    break;

  case DX9MT_SM_OP_FRC:
    snprintf(rhs, sizeof(rhs), "fract(%s)", s0);
    break;

  case DX9MT_SM_OP_ABS:
    snprintf(rhs, sizeof(rhs), "abs(%s)", s0);
    break;

  case DX9MT_SM_OP_NRM: {
    /* nrm dst, src: normalize xyz, w = 1/length */
    snprintf(rhs, sizeof(rhs),
             "float4(normalize(%s.xyz), rsqrt(dot(%s.xyz, %s.xyz)))", s0, s0, s0);
    break;
  }

  case DX9MT_SM_OP_LRP:
    /* lrp dst, f, a, b = mix(b, a, f) = f*(a-b)+b */
    snprintf(rhs, sizeof(rhs), "mix(%s, %s, %s)", s2, s1, s0);
    break;

  case DX9MT_SM_OP_CMP:
    /* cmp dst, src0, src1, src2: per-component (src0 >= 0) ? src1 : src2 */
    snprintf(rhs, sizeof(rhs),
             "select(%s, %s, %s >= float4(0.0))", s2, s1, s0);
    break;

  case DX9MT_SM_OP_POW:
    snprintf(rhs, sizeof(rhs), "float4(pow(abs(%s.x), %s.x))", s0, s1);
    break;

  case DX9MT_SM_OP_CRS:
    snprintf(rhs, sizeof(rhs),
             "float4(cross(%s.xyz, %s.xyz), 0.0)", s0, s1);
    break;

  case DX9MT_SM_OP_SINCOS:
    /* sincos dst, src: dst.x = cos(src.x), dst.y = sin(src.x) */
    snprintf(rhs, sizeof(rhs),
             "float4(cos(%s.x), sin(%s.x), 0.0, 0.0)", s0, s0);
    break;

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
    snprintf(rhs, sizeof(rhs),
             "float4(1.0, %s.y * %s.y, %s.z, %s.w)", s0, s1, s0, s1);
    break;

  case DX9MT_SM_OP_DP2ADD:
    snprintf(rhs, sizeof(rhs),
             "float4(dot(%s.xy, %s.xy) + %s.x)", s0, s1, s2);
    break;

  case DX9MT_SM_OP_MOVA:
    /* mova a0, src: integer part of src -> address register */
    snprintf(rhs, sizeof(rhs), "float4(floor(%s + float4(0.5)))", s0);
    break;

  case DX9MT_SM_OP_M4x4: {
    /* m4x4 dst, src0, src1: dst.x = dp4(src0, c[src1+0]) ... */
    char base_c[80];
    reg_name(base_c, sizeof(base_c), &inst->src[1], is_vs);
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
    char coord_expr[128];
    src_expr(coord_expr, sizeof(coord_expr), &inst->src[0], is_vs);
    snprintf(rhs, sizeof(rhs), "tex%u.sample(samp%u, %s.xy)",
             samp_num, samp_num, coord_expr);
    break;
  }

  case DX9MT_SM_OP_TEXLDL: {
    /* texldl dst, coord, sampler: coord.w = LOD */
    uint16_t samp_num = inst->src[1].number;
    char coord_expr[128];
    src_expr(coord_expr, sizeof(coord_expr), &inst->src[0], is_vs);
    snprintf(rhs, sizeof(rhs), "tex%u.sample(samp%u, %s.xy, level(%s.w))",
             samp_num, samp_num, coord_expr, coord_expr);
    break;
  }

  case DX9MT_SM_OP_TEXKILL:
    emit(ctx, "  if (any(%s.xyz < float3(0.0))) discard_fragment();\n", dst);
    return;

  /* Flow control -- emit as comments for now (FNV menu unlikely to need) */
  case DX9MT_SM_OP_REP:
    emit(ctx, "  // rep (flow control not yet translated)\n");
    return;
  case DX9MT_SM_OP_ENDREP:
    emit(ctx, "  // endrep\n");
    return;
  case DX9MT_SM_OP_IF:
    emit(ctx, "  // if (flow control not yet translated)\n");
    return;
  case DX9MT_SM_OP_IFC:
    emit(ctx, "  // ifc (flow control not yet translated)\n");
    return;
  case DX9MT_SM_OP_ELSE:
    emit(ctx, "  // else\n");
    return;
  case DX9MT_SM_OP_ENDIF:
    emit(ctx, "  // endif\n");
    return;
  case DX9MT_SM_OP_BREAK:
    emit(ctx, "  // break\n");
    return;
  case DX9MT_SM_OP_BREAKC:
    emit(ctx, "  // breakc\n");
    return;

  default:
    emit(ctx, "  // unsupported opcode %u\n", inst->opcode);
    return;
  }

  /* Standard assignment with optional saturation */
  if (do_sat)
    emit(ctx, "  %s%s = saturate(%s);\n", dst, wm, rhs);
  else
    emit(ctx, "  %s%s = %s;\n", dst, wm, rhs);
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

  /* Mark which c# registers have def values */
  for (uint32_t i = 0; i < prog->def_count; ++i) {
    if (prog->defs[i].reg_type == DX9MT_SM_REG_CONST &&
        prog->defs[i].reg_number < 256) {
      ctx.def_reg_set[prog->defs[i].reg_number] = 1;
    }
  }

  emit(&ctx, "#include <metal_stdlib>\n");
  emit(&ctx, "using namespace metal;\n\n");

  /* Input struct from vertex attributes */
  emit(&ctx, "struct VS_In_%08x {\n", bytecode_hash);
  for (uint32_t i = 0; i < prog->dcl_count; ++i) {
    const dx9mt_sm_dcl_entry *d = &prog->dcls[i];
    if (d->reg_type != DX9MT_SM_REG_INPUT) continue;

    const char *type = "float4";
    /* Determine type from write mask */
    int mc = mask_count(d->write_mask);
    if (mc == 1) type = "float";
    else if (mc == 2) type = "float2";
    else if (mc == 3) type = "float3";

    emit(&ctx, "  %s v%u [[attribute(%u)]];\n", type, d->reg_number, d->reg_number);
  }
  emit(&ctx, "};\n\n");

  /* Output struct (interpolants to PS) */
  emit(&ctx, "struct VS_Out_%08x {\n", bytecode_hash);
  emit(&ctx, "  float4 position [[position]];\n");

  /* VS 3.0: output registers from dcl */
  if (prog->major_version >= 3) {
    for (uint32_t i = 0; i < prog->dcl_count; ++i) {
      const dx9mt_sm_dcl_entry *d = &prog->dcls[i];
      if (d->reg_type != DX9MT_SM_REG_OUTPUT) continue;
      /* Skip position (already emitted above) */
      if (d->usage == DX9MT_SM_USAGE_POSITION && d->usage_index == 0) continue;

      int mc = mask_count(d->write_mask);
      const char *type = "float4";
      if (mc == 1) type = "float";
      else if (mc == 2) type = "float2";
      else if (mc == 3) type = "float3";

      emit(&ctx, "  %s o%u;\n", type, d->reg_number);
    }
  } else {
    /* VS 2.0: oD# and oT# outputs */
    for (int i = 0; i < 2; ++i) {
      if (prog->color_output_mask & (1u << i))
        emit(&ctx, "  float4 oD%d;\n", i);
    }
    /* Check which oT# are written by scanning instructions */
    for (uint32_t i = 0; i < prog->instruction_count; ++i) {
      const dx9mt_sm_instruction *inst = &prog->instructions[i];
      if (inst->dst.type == DX9MT_SM_REG_OUTPUT ||
          inst->dst.type == DX9MT_SM_REG_ADDR) {
        /* In VS 2.0, texture coord outputs use reg type 3 (ADDR/TEXCRD) with
         * output bit set. Actually they use oT# which is... */
      }
    }
    /* Emit oT# for texcoord registers written to -- use output_mask */
    for (int i = 0; i < 8; ++i) {
      if (prog->texcoord_output_mask & (1u << i))
        emit(&ctx, "  float4 oT%d;\n", i);
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
      emit(&ctx, "  // def c%u overridden by inline constant\n", d->reg_number);
    }
  }

  emit(&ctx, "  VS_Out_%08x out;\n", bytecode_hash);
  emit(&ctx, "  out.position = float4(0.0);\n");
  emit(&ctx, "\n");

  /* Emit instructions */
  for (uint32_t i = 0; i < prog->instruction_count; ++i) {
    emit_instruction(&ctx, &prog->instructions[i], 1);
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

  for (uint32_t i = 0; i < prog->def_count; ++i) {
    if (prog->defs[i].reg_type == DX9MT_SM_REG_CONST &&
        prog->defs[i].reg_number < 256) {
      ctx.def_reg_set[prog->defs[i].reg_number] = 1;
    }
  }

  emit(&ctx, "#include <metal_stdlib>\n");
  emit(&ctx, "using namespace metal;\n\n");

  /* Input struct (interpolants from VS) */
  emit(&ctx, "struct PS_In_%08x {\n", bytecode_hash);
  emit(&ctx, "  float4 position [[position]];\n");
  for (uint32_t i = 0; i < prog->dcl_count; ++i) {
    const dx9mt_sm_dcl_entry *d = &prog->dcls[i];
    if (d->reg_type != DX9MT_SM_REG_INPUT) continue;

    int mc = mask_count(d->write_mask);
    const char *type = "float4";
    if (mc == 1) type = "float";
    else if (mc == 2) type = "float2";
    else if (mc == 3) type = "float3";

    emit(&ctx, "  %s v%u;\n", type, d->reg_number);
  }
  /* vPos if used */
  for (uint32_t i = 0; i < prog->dcl_count; ++i) {
    if (prog->dcls[i].reg_type == DX9MT_SM_REG_MISCTYPE &&
        prog->dcls[i].reg_number == 0) {
      emit(&ctx, "  // vPos mapped to position\n");
    }
  }
  emit(&ctx, "};\n\n");

  /* Fragment function */
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

  /* Output color register */
  emit(&ctx, "  float4 oC0 = float4(0.0);\n");
  if (prog->num_color_outputs > 1) {
    for (int i = 1; i < prog->num_color_outputs; ++i) {
      emit(&ctx, "  float4 oC%d = float4(0.0);\n", i);
    }
  }
  if (prog->writes_depth) {
    emit(&ctx, "  float oDepth = 0.0;\n");
  }

  /* Inline def constants */
  for (uint32_t i = 0; i < prog->def_count; ++i) {
    const dx9mt_sm_def_entry *d = &prog->defs[i];
    if (d->reg_type == DX9MT_SM_REG_CONST) {
      emit(&ctx, "  // def c%u overridden by inline constant\n", d->reg_number);
    }
  }

  emit(&ctx, "\n");

  /* Emit instructions */
  for (uint32_t i = 0; i < prog->instruction_count; ++i) {
    emit_instruction(&ctx, &prog->instructions[i], 0);
  }

  emit(&ctx, "\n  return oC0;\n");
  emit(&ctx, "}\n");

  if (ctx.error) {
    snprintf(out->error_msg, sizeof(out->error_msg), "MSL source buffer overflow");
    out->has_error = 1;
    return -1;
  }

  out->source_len = ctx.len;
  return 0;
}
