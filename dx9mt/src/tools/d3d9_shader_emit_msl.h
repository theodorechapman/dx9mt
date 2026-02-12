#ifndef DX9MT_D3D9_SHADER_EMIT_MSL_H
#define DX9MT_D3D9_SHADER_EMIT_MSL_H

#include "d3d9_shader_parse.h"

#define DX9MT_MSL_MAX_SOURCE (32 * 1024)

typedef struct dx9mt_msl_emit_result {
  char source[DX9MT_MSL_MAX_SOURCE];
  uint32_t source_len;
  char entry_name[64];
  int has_error;
  char error_msg[128];
} dx9mt_msl_emit_result;

/* Emit MSL vertex function. Entry: "vs_XXXXXXXX". Returns 0 on success. */
int dx9mt_msl_emit_vs(const dx9mt_sm_program *prog, uint32_t bytecode_hash,
                      dx9mt_msl_emit_result *out);

/* Emit MSL fragment function. Entry: "ps_XXXXXXXX". Returns 0 on success. */
int dx9mt_msl_emit_ps(const dx9mt_sm_program *prog, uint32_t bytecode_hash,
                      dx9mt_msl_emit_result *out);

#endif
