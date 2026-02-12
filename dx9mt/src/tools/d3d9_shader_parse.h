#ifndef DX9MT_D3D9_SHADER_PARSE_H
#define DX9MT_D3D9_SHADER_PARSE_H

#include <stdint.h>
#include <stdio.h>

#define DX9MT_SM_MAX_INSTRUCTIONS 512
#define DX9MT_SM_MAX_SOURCES 4
#define DX9MT_SM_MAX_DCL 48
#define DX9MT_SM_MAX_DEF 64

/* D3D9 shader register types */
enum dx9mt_sm_reg_type {
  DX9MT_SM_REG_TEMP       = 0,
  DX9MT_SM_REG_INPUT      = 1,
  DX9MT_SM_REG_CONST      = 2,
  DX9MT_SM_REG_ADDR       = 3,  /* a0 (VS) / t# (PS < 2.0) */
  DX9MT_SM_REG_RASTOUT    = 4,  /* oPos=0, oFog=1, oPts=2 */
  DX9MT_SM_REG_ATTROUT    = 5,  /* oD# */
  DX9MT_SM_REG_OUTPUT     = 6,  /* o# (VS 3.0) */
  DX9MT_SM_REG_CONSTINT   = 7,
  DX9MT_SM_REG_COLOROUT   = 8,  /* oC# */
  DX9MT_SM_REG_DEPTHOUT   = 9,  /* oDepth */
  DX9MT_SM_REG_SAMPLER    = 10,
  DX9MT_SM_REG_CONST2     = 11,
  DX9MT_SM_REG_CONST3     = 12,
  DX9MT_SM_REG_CONST4     = 13,
  DX9MT_SM_REG_CONSTBOOL  = 14,
  DX9MT_SM_REG_LOOP       = 15,
  DX9MT_SM_REG_TEMPFLOAT16= 16,
  DX9MT_SM_REG_MISCTYPE   = 17,  /* vPos=0, vFace=1 */
  DX9MT_SM_REG_LABEL      = 18,
  DX9MT_SM_REG_PREDICATE  = 19,
};

/* D3D9 shader opcodes */
enum dx9mt_sm_opcode {
  DX9MT_SM_OP_NOP     = 0,
  DX9MT_SM_OP_MOV     = 1,
  DX9MT_SM_OP_ADD     = 2,
  DX9MT_SM_OP_SUB     = 3,
  DX9MT_SM_OP_MAD     = 4,
  DX9MT_SM_OP_MUL     = 5,
  DX9MT_SM_OP_RCP     = 6,
  DX9MT_SM_OP_RSQ     = 7,
  DX9MT_SM_OP_DP3     = 8,
  DX9MT_SM_OP_DP4     = 9,
  DX9MT_SM_OP_MIN     = 10,
  DX9MT_SM_OP_MAX     = 11,
  DX9MT_SM_OP_SLT     = 12,
  DX9MT_SM_OP_SGE     = 13,
  DX9MT_SM_OP_EXP     = 14,
  DX9MT_SM_OP_LOG     = 15,
  DX9MT_SM_OP_LIT     = 16,
  DX9MT_SM_OP_DST     = 17,
  DX9MT_SM_OP_LRP     = 18,
  DX9MT_SM_OP_FRC     = 19,
  DX9MT_SM_OP_M4x4    = 20,
  DX9MT_SM_OP_M4x3    = 21,
  DX9MT_SM_OP_M3x4    = 22,
  DX9MT_SM_OP_M3x3    = 23,
  DX9MT_SM_OP_M3x2    = 24,
  DX9MT_SM_OP_DCL     = 31,
  DX9MT_SM_OP_POW     = 32,
  DX9MT_SM_OP_CRS     = 33,
  DX9MT_SM_OP_SGN     = 34,
  DX9MT_SM_OP_ABS     = 35,
  DX9MT_SM_OP_NRM     = 36,
  DX9MT_SM_OP_SINCOS  = 37,
  DX9MT_SM_OP_REP     = 38,
  DX9MT_SM_OP_ENDREP  = 39,
  DX9MT_SM_OP_IF      = 40,
  DX9MT_SM_OP_IFC     = 41,
  DX9MT_SM_OP_ELSE    = 42,
  DX9MT_SM_OP_ENDIF   = 43,
  DX9MT_SM_OP_BREAK   = 44,
  DX9MT_SM_OP_BREAKC  = 45,
  DX9MT_SM_OP_MOVA    = 46,
  DX9MT_SM_OP_TEXKILL = 65,
  DX9MT_SM_OP_TEXLD   = 66,
  DX9MT_SM_OP_TEXLDL  = 67,
  DX9MT_SM_OP_DEF     = 81,
  DX9MT_SM_OP_DEFI    = 82,
  DX9MT_SM_OP_DEFB    = 83,
  DX9MT_SM_OP_CMP     = 88,
  DX9MT_SM_OP_DP2ADD  = 112,
  DX9MT_SM_OP_END     = 0xFFFF,
};

/* Source modifiers */
enum dx9mt_sm_src_mod {
  DX9MT_SM_SRCMOD_NONE       = 0,
  DX9MT_SM_SRCMOD_NEGATE     = 1,
  DX9MT_SM_SRCMOD_BIAS       = 2,
  DX9MT_SM_SRCMOD_BIAS_NEG   = 3,
  DX9MT_SM_SRCMOD_SIGN       = 4,
  DX9MT_SM_SRCMOD_SIGN_NEG   = 5,
  DX9MT_SM_SRCMOD_COMPLEMENT = 6,
  DX9MT_SM_SRCMOD_X2         = 7,
  DX9MT_SM_SRCMOD_X2_NEG     = 8,
  DX9MT_SM_SRCMOD_DZ         = 9,
  DX9MT_SM_SRCMOD_DW         = 10,
  DX9MT_SM_SRCMOD_ABS        = 11,
  DX9MT_SM_SRCMOD_ABS_NEG    = 12,
  DX9MT_SM_SRCMOD_NOT        = 13,
};

/* Result (destination) modifiers */
enum dx9mt_sm_result_mod {
  DX9MT_SM_RMOD_NONE     = 0,
  DX9MT_SM_RMOD_SATURATE = 1,
  DX9MT_SM_RMOD_PP       = 2,  /* partial precision (ignored) */
  DX9MT_SM_RMOD_CENTROID = 4,  /* centroid (ignored) */
};

/* Sampler type for dcl */
enum dx9mt_sm_sampler_type {
  DX9MT_SM_SAMP_2D     = 2,
  DX9MT_SM_SAMP_CUBE   = 3,
  DX9MT_SM_SAMP_VOLUME = 4,
};

/* DCL usage semantics */
enum dx9mt_sm_dcl_usage {
  DX9MT_SM_USAGE_POSITION     = 0,
  DX9MT_SM_USAGE_BLENDWEIGHT  = 1,
  DX9MT_SM_USAGE_BLENDINDICES = 2,
  DX9MT_SM_USAGE_NORMAL       = 3,
  DX9MT_SM_USAGE_PSIZE        = 4,
  DX9MT_SM_USAGE_TEXCOORD     = 5,
  DX9MT_SM_USAGE_TANGENT      = 6,
  DX9MT_SM_USAGE_BINORMAL     = 7,
  DX9MT_SM_USAGE_TESSFACTOR   = 8,
  DX9MT_SM_USAGE_POSITIONT    = 9,
  DX9MT_SM_USAGE_COLOR        = 10,
  DX9MT_SM_USAGE_FOG          = 11,
  DX9MT_SM_USAGE_DEPTH        = 12,
  DX9MT_SM_USAGE_SAMPLE       = 13,
};

typedef struct dx9mt_sm_register {
  uint16_t type;         /* dx9mt_sm_reg_type */
  uint16_t number;
  uint8_t  swizzle[4];   /* 0=x, 1=y, 2=z, 3=w */
  uint8_t  write_mask;   /* bitmask: bit0=x, bit1=y, bit2=z, bit3=w */
  uint8_t  src_modifier;  /* dx9mt_sm_src_mod */
  uint8_t  result_modifier; /* dx9mt_sm_result_mod */
  uint8_t  has_relative;
} dx9mt_sm_register;

typedef struct dx9mt_sm_instruction {
  uint16_t opcode;
  uint8_t  num_sources;
  uint8_t  comparison;   /* for ifc/breakc: comparison type */
  dx9mt_sm_register dst;
  dx9mt_sm_register src[DX9MT_SM_MAX_SOURCES];
} dx9mt_sm_instruction;

typedef struct dx9mt_sm_dcl_entry {
  uint8_t  usage;        /* dx9mt_sm_dcl_usage */
  uint8_t  usage_index;
  uint8_t  reg_type;
  uint8_t  write_mask;
  uint16_t reg_number;
  uint16_t sampler_type; /* dx9mt_sm_sampler_type (sampler dcls only) */
} dx9mt_sm_dcl_entry;

typedef struct dx9mt_sm_def_entry {
  uint16_t reg_type;     /* CONST, CONSTINT, CONSTBOOL */
  uint16_t reg_number;
  union {
    float    f[4];
    int32_t  i[4];
    uint32_t b;
  } values;
} dx9mt_sm_def_entry;

typedef struct dx9mt_sm_program {
  uint8_t  shader_type;   /* 0=pixel, 1=vertex */
  uint8_t  major_version;
  uint8_t  minor_version;
  uint8_t  _pad;

  uint32_t instruction_count;
  dx9mt_sm_instruction instructions[DX9MT_SM_MAX_INSTRUCTIONS];

  uint32_t dcl_count;
  dx9mt_sm_dcl_entry dcls[DX9MT_SM_MAX_DCL];

  uint32_t def_count;
  dx9mt_sm_def_entry defs[DX9MT_SM_MAX_DEF];

  /* Analysis: which registers are used */
  uint32_t max_temp_reg;
  uint32_t max_const_reg;
  uint32_t sampler_mask;   /* bitmask of s# used in texld */
  uint32_t input_mask;     /* bitmask of v# declared/used */
  uint32_t output_mask;    /* bitmask of o# declared/used */
  uint32_t texcoord_output_mask; /* bitmask of oT# written (VS < 3.0) */
  uint32_t color_output_mask;    /* bitmask of oD# written (VS < 3.0) */
  int      writes_position;      /* 1 if oPos / o# with POSITION written */
  int      writes_fog;
  int      writes_depth;         /* oDepth */
  int      num_color_outputs;    /* PS: number of oC# written */

  int      has_error;
  char     error_msg[128];
} dx9mt_sm_program;

/* Parse shader bytecode into program IR. Returns 0 on success. */
int dx9mt_sm_parse(const uint32_t *bytecode, uint32_t dword_count,
                   dx9mt_sm_program *out);

/* FNV-1a hash of bytecode for cache keying. */
uint32_t dx9mt_sm_bytecode_hash(const uint32_t *bytecode, uint32_t dword_count);

/* Debug: print parsed program summary to file. */
void dx9mt_sm_dump(const dx9mt_sm_program *prog, FILE *f);

#endif
