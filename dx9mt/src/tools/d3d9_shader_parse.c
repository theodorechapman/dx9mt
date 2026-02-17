#include "d3d9_shader_parse.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Token decoding helpers                                              */
/* ------------------------------------------------------------------ */

static uint16_t decode_reg_type(uint32_t token) {
  return (uint16_t)(((token >> 28) & 0x7u) | (((token >> 11) & 0x3u) << 3));
}

static uint16_t decode_reg_number(uint32_t token) {
  return (uint16_t)(token & 0x7FFu);
}

static dx9mt_sm_register decode_dst(uint32_t token) {
  dx9mt_sm_register r;
  memset(&r, 0, sizeof(r));
  r.type = decode_reg_type(token);
  r.number = decode_reg_number(token);
  r.write_mask = (uint8_t)((token >> 16) & 0xFu);
  r.result_modifier = (uint8_t)((token >> 20) & 0xFu);
  /* Default swizzle to identity for dst (not used but keeps struct sane) */
  r.swizzle[0] = 0; r.swizzle[1] = 1; r.swizzle[2] = 2; r.swizzle[3] = 3;
  return r;
}

static dx9mt_sm_register decode_src(uint32_t token) {
  dx9mt_sm_register r;
  memset(&r, 0, sizeof(r));
  r.type = decode_reg_type(token);
  r.number = decode_reg_number(token);
  r.swizzle[0] = (uint8_t)((token >> 16) & 0x3u);
  r.swizzle[1] = (uint8_t)((token >> 18) & 0x3u);
  r.swizzle[2] = (uint8_t)((token >> 20) & 0x3u);
  r.swizzle[3] = (uint8_t)((token >> 22) & 0x3u);
  r.src_modifier = (uint8_t)((token >> 24) & 0xFu);
  r.has_relative = (uint8_t)((token >> 13) & 0x1u);
  r.write_mask = 0xF; /* sources conceptually use all components */
  return r;
}

/* ------------------------------------------------------------------ */
/* Opcode metadata                                                     */
/* ------------------------------------------------------------------ */

/*
 * Returns number of source registers for an opcode.
 * -1 = unknown opcode, -2 = special handling (dcl/def/end).
 */
static int opcode_src_count(uint16_t op) {
  switch (op) {
  case DX9MT_SM_OP_NOP:     return 0;
  case DX9MT_SM_OP_MOV:     return 1;
  case DX9MT_SM_OP_ADD:     return 2;
  case DX9MT_SM_OP_SUB:     return 2;
  case DX9MT_SM_OP_MAD:     return 3;
  case DX9MT_SM_OP_MUL:     return 2;
  case DX9MT_SM_OP_RCP:     return 1;
  case DX9MT_SM_OP_RSQ:     return 1;
  case DX9MT_SM_OP_DP3:     return 2;
  case DX9MT_SM_OP_DP4:     return 2;
  case DX9MT_SM_OP_MIN:     return 2;
  case DX9MT_SM_OP_MAX:     return 2;
  case DX9MT_SM_OP_SLT:     return 2;
  case DX9MT_SM_OP_SGE:     return 2;
  case DX9MT_SM_OP_EXP:     return 1;
  case DX9MT_SM_OP_LOG:     return 1;
  case DX9MT_SM_OP_LIT:     return 1;
  case DX9MT_SM_OP_DST:     return 2;
  case DX9MT_SM_OP_LRP:     return 3;
  case DX9MT_SM_OP_FRC:     return 1;
  case DX9MT_SM_OP_M4x4:    return 2;
  case DX9MT_SM_OP_M4x3:    return 2;
  case DX9MT_SM_OP_M3x4:    return 2;
  case DX9MT_SM_OP_M3x3:    return 2;
  case DX9MT_SM_OP_M3x2:    return 2;
  case DX9MT_SM_OP_POW:     return 2;
  case DX9MT_SM_OP_CRS:     return 2;
  case DX9MT_SM_OP_SGN:     return 3;
  case DX9MT_SM_OP_ABS:     return 1;
  case DX9MT_SM_OP_NRM:     return 1;
  case DX9MT_SM_OP_SINCOS:  return 1; /* SM3.0: 1 src */
  case DX9MT_SM_OP_MOVA:    return 1;
  case DX9MT_SM_OP_TEXKILL: return 0; /* dst only */
  case DX9MT_SM_OP_TEXLD:   return 2; /* coord, sampler */
  case DX9MT_SM_OP_TEXLDL:  return 2; /* coord, sampler */
  case DX9MT_SM_OP_CMP:     return 3;
  case DX9MT_SM_OP_DP2ADD:  return 3;
  /* Flow control (no dst/src in normal sense) */
  case DX9MT_SM_OP_REP:     return -2;
  case DX9MT_SM_OP_ENDREP:  return -2;
  case DX9MT_SM_OP_IF:      return -2;
  case DX9MT_SM_OP_IFC:     return -2;
  case DX9MT_SM_OP_ELSE:    return -2;
  case DX9MT_SM_OP_ENDIF:   return -2;
  case DX9MT_SM_OP_BREAK:   return -2;
  case DX9MT_SM_OP_BREAKC:  return -2;
  /* Special */
  case DX9MT_SM_OP_DCL:     return -2;
  case DX9MT_SM_OP_DEF:     return -2;
  case DX9MT_SM_OP_DEFI:    return -2;
  case DX9MT_SM_OP_DEFB:    return -2;
  case DX9MT_SM_OP_END:     return -2;
  default:                   return -1;
  }
}

static int opcode_has_dst(uint16_t op) {
  switch (op) {
  case DX9MT_SM_OP_NOP:
  case DX9MT_SM_OP_REP:
  case DX9MT_SM_OP_ENDREP:
  case DX9MT_SM_OP_IF:
  case DX9MT_SM_OP_ELSE:
  case DX9MT_SM_OP_ENDIF:
  case DX9MT_SM_OP_BREAK:
  case DX9MT_SM_OP_END:
    return 0;
  default:
    return 1;
  }
}

/* ------------------------------------------------------------------ */
/* Register usage tracking                                             */
/* ------------------------------------------------------------------ */

static void track_register_usage(dx9mt_sm_program *prog,
                                 const dx9mt_sm_register *reg,
                                 int is_dst) {
  if (!prog || !reg) {
    return;
  }

  switch (reg->type) {
  case DX9MT_SM_REG_TEMP:
    if (reg->number > 255u) {
      snprintf(prog->error_msg, sizeof(prog->error_msg),
               "temp register %u out of supported range", reg->number);
      prog->has_error = 1;
      return;
    }
    if (reg->number > prog->max_temp_reg)
      prog->max_temp_reg = reg->number;
    break;
  case DX9MT_SM_REG_CONST:
    if (reg->number > 255u) {
      snprintf(prog->error_msg, sizeof(prog->error_msg),
               "const register %u out of supported range", reg->number);
      prog->has_error = 1;
      return;
    }
    if (reg->number > prog->max_const_reg)
      prog->max_const_reg = reg->number;
    break;
  case DX9MT_SM_REG_INPUT:
    if (reg->number >= 32u) {
      snprintf(prog->error_msg, sizeof(prog->error_msg),
               "input register %u out of supported range", reg->number);
      prog->has_error = 1;
      return;
    }
    prog->input_mask |= (1u << reg->number);
    break;
  case DX9MT_SM_REG_OUTPUT:
    if (reg->number >= 32u) {
      snprintf(prog->error_msg, sizeof(prog->error_msg),
               "output register %u out of supported range", reg->number);
      prog->has_error = 1;
      return;
    }
    prog->output_mask |= (1u << reg->number);
    break;
  case DX9MT_SM_REG_SAMPLER:
    if (reg->number >= 32u) {
      snprintf(prog->error_msg, sizeof(prog->error_msg),
               "sampler register %u out of supported range", reg->number);
      prog->has_error = 1;
      return;
    }
    prog->sampler_mask |= (1u << reg->number);
    break;
  case DX9MT_SM_REG_RASTOUT:
    if (reg->number > 2u) {
      snprintf(prog->error_msg, sizeof(prog->error_msg),
               "rastout register %u out of supported range", reg->number);
      prog->has_error = 1;
      return;
    }
    if (is_dst && reg->number == 0)
      prog->writes_position = 1;
    if (is_dst && reg->number == 1)
      prog->writes_fog = 1;
    break;
  case DX9MT_SM_REG_ATTROUT:
    if (reg->number >= 32u) {
      snprintf(prog->error_msg, sizeof(prog->error_msg),
               "attribute output register %u out of supported range", reg->number);
      prog->has_error = 1;
      return;
    }
    if (is_dst)
      prog->color_output_mask |= (1u << reg->number);
    break;
  case DX9MT_SM_REG_COLOROUT:
    if (reg->number >= 32u) {
      snprintf(prog->error_msg, sizeof(prog->error_msg),
               "color output register %u out of supported range", reg->number);
      prog->has_error = 1;
      return;
    }
    if (is_dst) {
      prog->num_color_outputs = (int)reg->number + 1;
    }
    break;
  case DX9MT_SM_REG_DEPTHOUT:
    if (is_dst)
      prog->writes_depth = 1;
    break;
  default:
    break;
  }
}

/* ------------------------------------------------------------------ */
/* Main parser                                                         */
/* ------------------------------------------------------------------ */

int dx9mt_sm_parse(const uint32_t *bytecode, uint32_t dword_count,
                   dx9mt_sm_program *out) {
  uint32_t pos = 0;
  int saw_end = 0;

  memset(out, 0, sizeof(*out));

  if (!bytecode || dword_count < 2) {
    snprintf(out->error_msg, sizeof(out->error_msg), "bytecode too short");
    out->has_error = 1;
    return -1;
  }

  /* Version token */
  uint32_t version = bytecode[pos++];
  out->minor_version = (uint8_t)(version & 0xFFu);
  out->major_version = (uint8_t)((version >> 8) & 0xFFu);

  if ((version & 0xFFFF0000u) == 0xFFFE0000u) {
    out->shader_type = 1; /* vertex */
  } else if ((version & 0xFFFF0000u) == 0xFFFF0000u) {
    out->shader_type = 0; /* pixel */
  } else {
    snprintf(out->error_msg, sizeof(out->error_msg),
             "bad version: 0x%08x", version);
    out->has_error = 1;
    return -1;
  }

  /* Instruction stream */
  while (pos < dword_count) {
    uint32_t instr_token = bytecode[pos];
    uint16_t opcode = (uint16_t)(instr_token & 0xFFFFu);

    /* End marker */
    if (opcode == DX9MT_SM_OP_END) {
      saw_end = 1;
      break;
    }

    /* Comment block: lower 16 bits = 0xFFFE, upper 15 bits = length in DWORDs */
    if ((instr_token & 0xFFFFu) == 0xFFFEu) {
      uint32_t comment_len = (instr_token >> 16) & 0x7FFFu;
      if (pos + 1 + comment_len > dword_count) {
        snprintf(out->error_msg, sizeof(out->error_msg),
                 "truncated comment block at dword %u", pos);
        out->has_error = 1;
        return -1;
      }
      pos += 1 + comment_len;
      continue;
    }

    pos++; /* consume instruction token */

    /* Instruction length from bits [27:24] (SM2+: additional DWORDs) */
    /* For SM2/3, the instruction length is encoded in the instruction token
     * for certain opcodes. But in practice, we count dst + srcs. */

    /* DCL: semantic_token + register_token */
    if (opcode == DX9MT_SM_OP_DCL) {
      if (pos + 2 > dword_count) {
        snprintf(out->error_msg, sizeof(out->error_msg),
                 "truncated dcl at dword %u", pos);
        out->has_error = 1;
        return -1;
      }
      uint32_t sem_token = bytecode[pos++];
      uint32_t reg_token = bytecode[pos++];

      if (out->dcl_count < DX9MT_SM_MAX_DCL) {
        dx9mt_sm_dcl_entry *dcl = &out->dcls[out->dcl_count++];
        dcl->usage = (uint8_t)(sem_token & 0x1Fu);
        dcl->usage_index = (uint8_t)((sem_token >> 16) & 0xFu);
        dcl->reg_type = (uint8_t)decode_reg_type(reg_token);
        dcl->reg_number = decode_reg_number(reg_token);
        dcl->write_mask = (uint8_t)((reg_token >> 16) & 0xFu);
        /* For sampler declarations, the semantic token encodes sampler type */
        if (dcl->reg_type == DX9MT_SM_REG_SAMPLER) {
          dcl->sampler_type = (uint16_t)((sem_token >> 27) & 0xFu);
          out->sampler_mask |= (1u << dcl->reg_number);
        }
        /* Track input/output declarations */
        if (dcl->reg_type == DX9MT_SM_REG_INPUT) {
          if (dcl->reg_number >= 32u) {
            snprintf(out->error_msg, sizeof(out->error_msg),
                     "invalid input register %u", dcl->reg_number);
            out->has_error = 1;
            return -1;
          }
          out->input_mask |= (1u << dcl->reg_number);
        }
        if (dcl->reg_type == DX9MT_SM_REG_OUTPUT) {
          if (dcl->reg_number >= 32u) {
            snprintf(out->error_msg, sizeof(out->error_msg),
                     "invalid output register %u", dcl->reg_number);
            out->has_error = 1;
            return -1;
          }
          out->output_mask |= (1u << dcl->reg_number);
        }
      } else {
        snprintf(out->error_msg, sizeof(out->error_msg),
                 "too many declarations (>%u)", DX9MT_SM_MAX_DCL);
        out->has_error = 1;
        return -1;
      }
      continue;
    }

    /* DEF: dst_token + 4 float immediates */
    if (opcode == DX9MT_SM_OP_DEF) {
      if (pos + 5 > dword_count) {
        snprintf(out->error_msg, sizeof(out->error_msg),
                 "truncated def at dword %u", pos);
        out->has_error = 1;
        return -1;
      }
      uint32_t dst_token = bytecode[pos++];
      if (out->def_count < DX9MT_SM_MAX_DEF) {
        dx9mt_sm_def_entry *def = &out->defs[out->def_count++];
        def->reg_type = DX9MT_SM_REG_CONST;
        def->reg_number = decode_reg_number(dst_token);
        memcpy(def->values.f, &bytecode[pos], 16);
      } else {
        snprintf(out->error_msg, sizeof(out->error_msg),
                 "too many immediate defs (>%u)", DX9MT_SM_MAX_DEF);
        out->has_error = 1;
        return -1;
      }
      pos += 4;
      continue;
    }

    /* DEFI: dst_token + 4 int immediates */
    if (opcode == DX9MT_SM_OP_DEFI) {
      if (pos + 5 > dword_count) {
        snprintf(out->error_msg, sizeof(out->error_msg),
                 "truncated defi at dword %u", pos);
        out->has_error = 1;
        return -1;
      }
      uint32_t dst_token = bytecode[pos++];
      if (out->def_count < DX9MT_SM_MAX_DEF) {
        dx9mt_sm_def_entry *def = &out->defs[out->def_count++];
        def->reg_type = DX9MT_SM_REG_CONSTINT;
        def->reg_number = decode_reg_number(dst_token);
        memcpy(def->values.i, &bytecode[pos], 16);
      } else {
        snprintf(out->error_msg, sizeof(out->error_msg),
                 "too many immediate defs (>%u)", DX9MT_SM_MAX_DEF);
        out->has_error = 1;
        return -1;
      }
      pos += 4;
      continue;
    }

    /* DEFB: dst_token + 1 bool */
    if (opcode == DX9MT_SM_OP_DEFB) {
      if (pos + 2 > dword_count) {
        snprintf(out->error_msg, sizeof(out->error_msg),
                 "truncated defb at dword %u", pos);
        out->has_error = 1;
        return -1;
      }
      uint32_t dst_token = bytecode[pos++];
      if (out->def_count < DX9MT_SM_MAX_DEF) {
        dx9mt_sm_def_entry *def = &out->defs[out->def_count++];
        def->reg_type = DX9MT_SM_REG_CONSTBOOL;
        def->reg_number = decode_reg_number(dst_token);
        def->values.b = bytecode[pos];
      } else {
        snprintf(out->error_msg, sizeof(out->error_msg),
                 "too many immediate defs (>%u)", DX9MT_SM_MAX_DEF);
        out->has_error = 1;
        return -1;
      }
      pos += 1;
      continue;
    }

    /* Flow control: ifc src0, src1 (comparison in instruction token bits 18-20) */
    if (opcode == DX9MT_SM_OP_IFC || opcode == DX9MT_SM_OP_BREAKC) {
      if (pos + 2 > dword_count) {
        snprintf(out->error_msg, sizeof(out->error_msg),
                 "truncated %s at dword %u",
                 opcode == DX9MT_SM_OP_IFC ? "ifc" : "breakc", pos);
        out->has_error = 1;
        return -1;
      }
      if (out->instruction_count >= DX9MT_SM_MAX_INSTRUCTIONS) {
        snprintf(out->error_msg, sizeof(out->error_msg),
                 "too many instructions (>%u)", DX9MT_SM_MAX_INSTRUCTIONS);
        out->has_error = 1;
        return -1;
      }
      dx9mt_sm_instruction *inst = &out->instructions[out->instruction_count++];
      memset(inst, 0, sizeof(*inst));
      inst->opcode = opcode;
      inst->comparison = (uint8_t)((instr_token >> 18) & 0x7u);
      inst->num_sources = 2;
      inst->src[0] = decode_src(bytecode[pos++]);
      inst->src[1] = decode_src(bytecode[pos++]);
      continue;
    }

    /* Flow control: rep i# / if b# (one source token) */
    if (opcode == DX9MT_SM_OP_REP || opcode == DX9MT_SM_OP_IF) {
      if (pos + 1 > dword_count) {
        snprintf(out->error_msg, sizeof(out->error_msg),
                 "truncated %s at dword %u",
                 opcode == DX9MT_SM_OP_REP ? "rep" : "if", pos);
        out->has_error = 1;
        return -1;
      }
      if (out->instruction_count >= DX9MT_SM_MAX_INSTRUCTIONS) {
        snprintf(out->error_msg, sizeof(out->error_msg),
                 "too many instructions (>%u)", DX9MT_SM_MAX_INSTRUCTIONS);
        out->has_error = 1;
        return -1;
      }
      dx9mt_sm_instruction *inst = &out->instructions[out->instruction_count++];
      memset(inst, 0, sizeof(*inst));
      inst->opcode = opcode;
      inst->num_sources = 1;
      inst->src[0] = decode_src(bytecode[pos++]);
      continue;
    }

    /* Flow control: else / endif / endrep / break (no operands) */
    if (opcode == DX9MT_SM_OP_ELSE || opcode == DX9MT_SM_OP_ENDIF ||
        opcode == DX9MT_SM_OP_ENDREP || opcode == DX9MT_SM_OP_BREAK) {
      if (out->instruction_count >= DX9MT_SM_MAX_INSTRUCTIONS) {
        snprintf(out->error_msg, sizeof(out->error_msg),
                 "too many instructions (>%u)", DX9MT_SM_MAX_INSTRUCTIONS);
        out->has_error = 1;
        return -1;
      }
      dx9mt_sm_instruction *inst = &out->instructions[out->instruction_count++];
      memset(inst, 0, sizeof(*inst));
      inst->opcode = opcode;
      inst->num_sources = 0;
      continue;
    }

    /* Regular arithmetic/texture instructions */
    int src_count = opcode_src_count(opcode);
    if (src_count < 0) {
      /* Unknown opcode -- try to skip using instruction length if encoded */
      /* SM3.0 encodes additional length in bits [27:24] for some opcodes,
       * but the standard pattern is just dst + N sources. Skip conservatively. */
      snprintf(out->error_msg, sizeof(out->error_msg),
               "unknown opcode %u at dword %u", opcode, pos - 1);
      out->has_error = 1;
      return -1;
    }

    int has_dst = opcode_has_dst(opcode);
    uint32_t tokens_needed = (uint32_t)has_dst + (uint32_t)src_count;

    /* TEXKILL has dst but no sources */
    if (opcode == DX9MT_SM_OP_TEXKILL) {
      has_dst = 1;
      tokens_needed = 1;
    }

    if (pos + tokens_needed > dword_count) {
      snprintf(out->error_msg, sizeof(out->error_msg),
               "truncated instruction operands at dword %u", pos);
      out->has_error = 1;
      return -1;
    }

    if (out->instruction_count >= DX9MT_SM_MAX_INSTRUCTIONS) {
      snprintf(out->error_msg, sizeof(out->error_msg),
               "too many instructions (>%u)", DX9MT_SM_MAX_INSTRUCTIONS);
      out->has_error = 1;
      return -1;
    }

    dx9mt_sm_instruction *inst = &out->instructions[out->instruction_count++];
    memset(inst, 0, sizeof(*inst));
    inst->opcode = opcode;

    if (has_dst) {
      uint32_t dst_token = bytecode[pos++];
      inst->dst = decode_dst(dst_token);

      /* Parse relative addressing token if present */
      if ((dst_token >> 13) & 0x1u) {
        if (pos >= dword_count) {
          snprintf(out->error_msg, sizeof(out->error_msg),
                   "truncated dst relative token at dword %u", pos);
          out->has_error = 1;
          return -1;
        }
        uint32_t rel_token = bytecode[pos++];
        inst->dst.has_relative = 1;
        inst->dst.relative_component = (uint8_t)((rel_token >> 16) & 0x3u);
      }

      track_register_usage(out, &inst->dst, 1);
      if (out->has_error) return -1;

      /* Track texcoord outputs for VS < 3.0 */
      if (out->shader_type == 1) {
        if (inst->dst.type == DX9MT_SM_REG_OUTPUT &&
            out->major_version >= 3) {
          /* VS 3.0 output -- tracked via dcl */
        } else if (inst->dst.type == DX9MT_SM_REG_RASTOUT &&
                   inst->dst.number == 0) {
          out->writes_position = 1;
        }
      }
    }

    inst->num_sources = (uint8_t)src_count;
    for (int s = 0; s < src_count; ++s) {
      uint32_t src_token = bytecode[pos++];
      inst->src[s] = decode_src(src_token);
      /* Parse relative addressing token if present */
      if (inst->src[s].has_relative) {
        if (pos >= dword_count) {
          snprintf(out->error_msg, sizeof(out->error_msg),
                   "truncated src relative token at dword %u", pos);
          out->has_error = 1;
          return -1;
        }
        uint32_t rel_token = bytecode[pos++];
        inst->src[s].relative_component = (uint8_t)((rel_token >> 16) & 0x3u);
      }

      track_register_usage(out, &inst->src[s], 0);
      if (out->has_error) return -1;
    }
  }

  /* If no explicit color outputs counted for PS, default to 1 */
  if (out->shader_type == 0 && out->num_color_outputs == 0) {
    out->num_color_outputs = 1;
  }

  if (!saw_end) {
    snprintf(out->error_msg, sizeof(out->error_msg), "missing END opcode");
    out->has_error = 1;
    return -1;
  }

  return 0;
}

/* ------------------------------------------------------------------ */
/* Bytecode hash                                                       */
/* ------------------------------------------------------------------ */

uint32_t dx9mt_sm_bytecode_hash(const uint32_t *bytecode, uint32_t dword_count) {
  uint32_t hash = 2166136261u;
  for (uint32_t i = 0; i < dword_count; ++i) {
    hash ^= bytecode[i];
    hash *= 16777619u;
  }
  return hash;
}

/* ------------------------------------------------------------------ */
/* Debug dump                                                          */
/* ------------------------------------------------------------------ */

static const char *opcode_name(uint16_t op) {
  switch (op) {
  case DX9MT_SM_OP_NOP:     return "nop";
  case DX9MT_SM_OP_MOV:     return "mov";
  case DX9MT_SM_OP_ADD:     return "add";
  case DX9MT_SM_OP_SUB:     return "sub";
  case DX9MT_SM_OP_MAD:     return "mad";
  case DX9MT_SM_OP_MUL:     return "mul";
  case DX9MT_SM_OP_RCP:     return "rcp";
  case DX9MT_SM_OP_RSQ:     return "rsq";
  case DX9MT_SM_OP_DP3:     return "dp3";
  case DX9MT_SM_OP_DP4:     return "dp4";
  case DX9MT_SM_OP_MIN:     return "min";
  case DX9MT_SM_OP_MAX:     return "max";
  case DX9MT_SM_OP_SLT:     return "slt";
  case DX9MT_SM_OP_SGE:     return "sge";
  case DX9MT_SM_OP_EXP:     return "exp";
  case DX9MT_SM_OP_LOG:     return "log";
  case DX9MT_SM_OP_LIT:     return "lit";
  case DX9MT_SM_OP_DST:     return "dst";
  case DX9MT_SM_OP_LRP:     return "lrp";
  case DX9MT_SM_OP_FRC:     return "frc";
  case DX9MT_SM_OP_M4x4:    return "m4x4";
  case DX9MT_SM_OP_M4x3:    return "m4x3";
  case DX9MT_SM_OP_M3x4:    return "m3x4";
  case DX9MT_SM_OP_M3x3:    return "m3x3";
  case DX9MT_SM_OP_M3x2:    return "m3x2";
  case DX9MT_SM_OP_POW:     return "pow";
  case DX9MT_SM_OP_CRS:     return "crs";
  case DX9MT_SM_OP_SGN:     return "sgn";
  case DX9MT_SM_OP_ABS:     return "abs";
  case DX9MT_SM_OP_NRM:     return "nrm";
  case DX9MT_SM_OP_SINCOS:  return "sincos";
  case DX9MT_SM_OP_MOVA:    return "mova";
  case DX9MT_SM_OP_TEXKILL: return "texkill";
  case DX9MT_SM_OP_TEXLD:   return "texld";
  case DX9MT_SM_OP_TEXLDL:  return "texldl";
  case DX9MT_SM_OP_CMP:     return "cmp";
  case DX9MT_SM_OP_DP2ADD:  return "dp2add";
  case DX9MT_SM_OP_REP:     return "rep";
  case DX9MT_SM_OP_ENDREP:  return "endrep";
  case DX9MT_SM_OP_IF:      return "if";
  case DX9MT_SM_OP_IFC:     return "ifc";
  case DX9MT_SM_OP_ELSE:    return "else";
  case DX9MT_SM_OP_ENDIF:   return "endif";
  case DX9MT_SM_OP_BREAK:   return "break";
  case DX9MT_SM_OP_BREAKC:  return "breakc";
  default:                   return "???";
  }
}

static const char *reg_type_name(uint16_t t) {
  switch (t) {
  case DX9MT_SM_REG_TEMP:      return "r";
  case DX9MT_SM_REG_INPUT:     return "v";
  case DX9MT_SM_REG_CONST:     return "c";
  case DX9MT_SM_REG_ADDR:      return "a";
  case DX9MT_SM_REG_RASTOUT:   return "rast";
  case DX9MT_SM_REG_ATTROUT:   return "oD";
  case DX9MT_SM_REG_OUTPUT:    return "o";
  case DX9MT_SM_REG_CONSTINT:  return "i";
  case DX9MT_SM_REG_COLOROUT:  return "oC";
  case DX9MT_SM_REG_DEPTHOUT:  return "oDepth";
  case DX9MT_SM_REG_SAMPLER:   return "s";
  case DX9MT_SM_REG_CONSTBOOL: return "b";
  case DX9MT_SM_REG_LOOP:      return "aL";
  case DX9MT_SM_REG_MISCTYPE:  return "misc";
  case DX9MT_SM_REG_PREDICATE: return "p";
  default:                     return "?";
  }
}

static const char *usage_name(uint8_t u) {
  switch (u) {
  case DX9MT_SM_USAGE_POSITION:     return "POSITION";
  case DX9MT_SM_USAGE_BLENDWEIGHT:  return "BLENDWEIGHT";
  case DX9MT_SM_USAGE_BLENDINDICES: return "BLENDINDICES";
  case DX9MT_SM_USAGE_NORMAL:       return "NORMAL";
  case DX9MT_SM_USAGE_PSIZE:        return "PSIZE";
  case DX9MT_SM_USAGE_TEXCOORD:     return "TEXCOORD";
  case DX9MT_SM_USAGE_TANGENT:      return "TANGENT";
  case DX9MT_SM_USAGE_BINORMAL:     return "BINORMAL";
  case DX9MT_SM_USAGE_POSITIONT:    return "POSITIONT";
  case DX9MT_SM_USAGE_COLOR:        return "COLOR";
  case DX9MT_SM_USAGE_FOG:          return "FOG";
  case DX9MT_SM_USAGE_DEPTH:        return "DEPTH";
  default:                          return "?";
  }
}

static void dump_reg(FILE *f, const dx9mt_sm_register *r, int is_dst) {
  static const char comp[] = "xyzw";
  fprintf(f, "%s%u", reg_type_name(r->type), r->number);

  if (is_dst) {
    if (r->write_mask != 0xF) {
      fprintf(f, ".");
      if (r->write_mask & 1) fprintf(f, "%c", comp[0]);
      if (r->write_mask & 2) fprintf(f, "%c", comp[1]);
      if (r->write_mask & 4) fprintf(f, "%c", comp[2]);
      if (r->write_mask & 8) fprintf(f, "%c", comp[3]);
    }
    if (r->result_modifier & DX9MT_SM_RMOD_SATURATE)
      fprintf(f, "_sat");
  } else {
    /* Source: show swizzle if not identity */
    if (r->swizzle[0] != 0 || r->swizzle[1] != 1 ||
        r->swizzle[2] != 2 || r->swizzle[3] != 3) {
      fprintf(f, ".%c%c%c%c",
              comp[r->swizzle[0]], comp[r->swizzle[1]],
              comp[r->swizzle[2]], comp[r->swizzle[3]]);
    }
    if (r->src_modifier == DX9MT_SM_SRCMOD_NEGATE) fprintf(f, " [neg]");
    else if (r->src_modifier == DX9MT_SM_SRCMOD_ABS) fprintf(f, " [abs]");
    else if (r->src_modifier == DX9MT_SM_SRCMOD_ABS_NEG) fprintf(f, " [abs_neg]");
    else if (r->src_modifier != 0) fprintf(f, " [mod%u]", r->src_modifier);
  }
}

void dx9mt_sm_dump(const dx9mt_sm_program *prog, FILE *f) {
  fprintf(f, "%s_%u_%u  instructions=%u  dcls=%u  defs=%u\n",
          prog->shader_type ? "vs" : "ps",
          prog->major_version, prog->minor_version,
          prog->instruction_count, prog->dcl_count, prog->def_count);
  fprintf(f, "  temp_regs=0..%u  max_const=c%u  samplers=0x%x  inputs=0x%x  outputs=0x%x\n",
          prog->max_temp_reg, prog->max_const_reg,
          prog->sampler_mask, prog->input_mask, prog->output_mask);
  if (prog->has_error)
    fprintf(f, "  ERROR: %s\n", prog->error_msg);

  for (uint32_t i = 0; i < prog->dcl_count; ++i) {
    const dx9mt_sm_dcl_entry *d = &prog->dcls[i];
    fprintf(f, "  dcl_%s%u  %s%u",
            usage_name(d->usage), d->usage_index,
            reg_type_name(d->reg_type), d->reg_number);
    if (d->reg_type == DX9MT_SM_REG_SAMPLER) {
      const char *st = d->sampler_type == 2 ? "2d" :
                       d->sampler_type == 3 ? "cube" :
                       d->sampler_type == 4 ? "volume" : "?";
      fprintf(f, " (%s)", st);
    }
    fprintf(f, "\n");
  }

  for (uint32_t i = 0; i < prog->def_count; ++i) {
    const dx9mt_sm_def_entry *d = &prog->defs[i];
    if (d->reg_type == DX9MT_SM_REG_CONST) {
      fprintf(f, "  def c%u = (%.6f, %.6f, %.6f, %.6f)\n",
              d->reg_number, d->values.f[0], d->values.f[1],
              d->values.f[2], d->values.f[3]);
    } else if (d->reg_type == DX9MT_SM_REG_CONSTINT) {
      fprintf(f, "  defi i%u = (%d, %d, %d, %d)\n",
              d->reg_number, d->values.i[0], d->values.i[1],
              d->values.i[2], d->values.i[3]);
    } else {
      fprintf(f, "  defb b%u = %u\n", d->reg_number, d->values.b);
    }
  }

  for (uint32_t i = 0; i < prog->instruction_count; ++i) {
    const dx9mt_sm_instruction *inst = &prog->instructions[i];
    fprintf(f, "  %s", opcode_name(inst->opcode));

    if (opcode_has_dst(inst->opcode) && inst->opcode != DX9MT_SM_OP_TEXKILL) {
      fprintf(f, " ");
      dump_reg(f, &inst->dst, 1);
    } else if (inst->opcode == DX9MT_SM_OP_TEXKILL) {
      fprintf(f, " ");
      dump_reg(f, &inst->dst, 1);
    }

    for (uint8_t s = 0; s < inst->num_sources; ++s) {
      fprintf(f, ", ");
      dump_reg(f, &inst->src[s], 0);
    }
    fprintf(f, "\n");
  }
}
