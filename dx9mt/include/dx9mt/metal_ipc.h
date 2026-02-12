#ifndef DX9MT_METAL_IPC_H
#define DX9MT_METAL_IPC_H

#include <stdint.h>

/*
 * Shared memory IPC for PE DLL <-> native Metal viewer.
 *
 * Layout (16MB region):
 *   [0..header_size)            dx9mt_metal_ipc_header
 *   [header_size..draws_end)    dx9mt_metal_ipc_draw[draw_count]
 *   [bulk_data_offset..]        bulk VB/IB bytes referenced by draw entries
 *
 * The PE DLL writes the entire region on present(), then stores the
 * sequence number last with release semantics. The viewer polls the
 * sequence number with acquire semantics.
 */

#define DX9MT_METAL_IPC_MAGIC 0xDEAD9001u
#define DX9MT_METAL_IPC_PATH "/tmp/dx9mt_metal_frame.bin"
#define DX9MT_METAL_IPC_WIN_PATH "Z:\\tmp\\dx9mt_metal_frame.bin"
#define DX9MT_METAL_IPC_SIZE (16u * 1024u * 1024u)
#define DX9MT_METAL_IPC_MAX_DRAWS 256u

typedef struct dx9mt_metal_ipc_draw {
  uint32_t primitive_type;
  int32_t base_vertex;
  uint32_t min_vertex_index;
  uint32_t num_vertices;
  uint32_t start_index;
  uint32_t primitive_count;
  uint32_t render_target_id;
  uint32_t render_target_texture_id;
  uint32_t render_target_width;
  uint32_t render_target_height;
  uint32_t render_target_format;

  uint32_t viewport_x;
  uint32_t viewport_y;
  uint32_t viewport_width;
  uint32_t viewport_height;
  float viewport_min_z;
  float viewport_max_z;

  int32_t scissor_left;
  int32_t scissor_top;
  int32_t scissor_right;
  int32_t scissor_bottom;

  uint32_t fvf;
  uint32_t pixel_shader_id;
  uint32_t stream0_offset;
  uint32_t stream0_stride;
  uint32_t index_format;

  /* RB3 Phase 2A: concrete stage-0 texture + sampler state */
  uint32_t texture0_id;
  uint32_t texture0_generation;
  uint32_t texture0_format;
  uint32_t texture0_width;
  uint32_t texture0_height;
  uint32_t texture0_pitch;

  uint32_t sampler0_min_filter;
  uint32_t sampler0_mag_filter;
  uint32_t sampler0_mip_filter;
  uint32_t sampler0_address_u;
  uint32_t sampler0_address_v;
  uint32_t sampler0_address_w;

  /* RB3 Phase 2C: stage-0 fixed-function combiner state */
  uint32_t tss0_color_op;
  uint32_t tss0_color_arg1;
  uint32_t tss0_color_arg2;
  uint32_t tss0_alpha_op;
  uint32_t tss0_alpha_arg1;
  uint32_t tss0_alpha_arg2;
  uint32_t rs_texture_factor;

  /* RB3 Phase 2B: key render states for UI composition */
  uint32_t rs_alpha_blend_enable;
  uint32_t rs_src_blend;
  uint32_t rs_dest_blend;
  uint32_t rs_alpha_test_enable;
  uint32_t rs_alpha_ref;
  uint32_t rs_alpha_func;

  /* Offsets into bulk data region (relative to bulk_data_offset) */
  uint32_t vb_bulk_offset;
  uint32_t vb_bulk_size;
  uint32_t ib_bulk_offset;
  uint32_t ib_bulk_size;
  uint32_t texture0_bulk_offset;
  uint32_t texture0_bulk_size;

  /* Vertex declaration: D3DVERTEXELEMENT9 is 8 bytes each */
  uint32_t decl_bulk_offset;
  uint16_t decl_count;
  uint16_t _pad0;

  /* Shader constants (256 float4s = 4096 bytes each) */
  uint32_t vs_constants_bulk_offset;
  uint32_t vs_constants_size;
  uint32_t ps_constants_bulk_offset;
  uint32_t ps_constants_size;

  /* RB3 Phase 3: shader bytecode for translation */
  uint32_t vertex_shader_id;
  uint32_t vs_bytecode_bulk_offset;
  uint32_t vs_bytecode_bulk_size;
  uint32_t ps_bytecode_bulk_offset;
  uint32_t ps_bytecode_bulk_size;
} dx9mt_metal_ipc_draw;

typedef struct dx9mt_metal_ipc_header {
  uint32_t magic;
  volatile uint32_t sequence;
  uint32_t width;
  uint32_t height;
  uint32_t clear_color_argb;
  uint32_t clear_flags;
  float clear_z;
  uint32_t clear_stencil;
  int32_t have_clear;
  uint32_t draw_count;
  uint32_t replay_hash;
  uint32_t frame_id;
  uint32_t present_render_target_id;
  uint32_t bulk_data_offset;
  uint32_t bulk_data_used;
} dx9mt_metal_ipc_header;

/* Back-compat alias for code that only reads the header */
typedef dx9mt_metal_ipc_header dx9mt_metal_frame_data;

#endif
