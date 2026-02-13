#ifndef DX9MT_PACKETS_H
#define DX9MT_PACKETS_H

#include <stdint.h>

#include "dx9mt/upload_arena.h"

enum dx9mt_packet_type {
  DX9MT_PACKET_INVALID = 0,
  DX9MT_PACKET_INIT = 1,
  DX9MT_PACKET_BEGIN_FRAME = 2,
  DX9MT_PACKET_DRAW_INDEXED = 3,
  DX9MT_PACKET_PRESENT = 4,
  DX9MT_PACKET_SHUTDOWN = 5,
  DX9MT_PACKET_CLEAR = 6,
};

typedef struct dx9mt_packet_header {
  uint16_t type;
  uint16_t size;
  uint32_t sequence;
} dx9mt_packet_header;

typedef struct dx9mt_packet_init {
  dx9mt_packet_header header;
  uint32_t protocol_version;
  uint32_t ring_capacity_bytes;
  dx9mt_upload_arena_desc upload_desc;
} dx9mt_packet_init;

typedef struct dx9mt_packet_begin_frame {
  dx9mt_packet_header header;
  uint32_t frame_id;
} dx9mt_packet_begin_frame;

typedef struct dx9mt_packet_draw_indexed {
  dx9mt_packet_header header;
  uint32_t state_block_hash;
  uint32_t primitive_type;
  int32_t base_vertex;
  uint32_t min_vertex_index;
  uint32_t num_vertices;
  uint32_t start_index;
  uint32_t primitive_count;
  uint32_t render_target_id;
  uint32_t depth_stencil_id;
  uint32_t render_target_texture_id;
  uint32_t render_target_width;
  uint32_t render_target_height;
  uint32_t render_target_format;
  uint32_t vertex_buffer_id;
  uint32_t index_buffer_id;
  uint32_t vertex_decl_id;
  uint32_t vertex_shader_id;
  uint32_t pixel_shader_id;
  uint32_t fvf;
  uint32_t stream0_offset;
  uint32_t stream0_stride;
  uint32_t viewport_hash;
  uint32_t scissor_hash;
  uint32_t texture_stage_hash;
  uint32_t sampler_state_hash;
  uint32_t stream_binding_hash;

  /* RB3 Phase 2A: concrete stage-0 texture + sampler state */
  uint32_t texture0_id;
  uint32_t texture0_generation;
  uint32_t texture0_format;
  uint32_t texture0_width;
  uint32_t texture0_height;
  uint32_t texture0_pitch;
  dx9mt_upload_ref texture0_data;

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

  dx9mt_upload_ref constants_vs;
  dx9mt_upload_ref constants_ps;

  /* RB3: actual viewport/scissor values (previously only hashes) */
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

  /* RB3: geometry data refs (VB/IB bytes, vertex declaration) */
  dx9mt_upload_ref vertex_data;
  uint32_t vertex_data_size;
  dx9mt_upload_ref index_data;
  uint32_t index_data_size;
  uint32_t index_format;
  dx9mt_upload_ref vertex_decl_data;
  uint16_t vertex_decl_count;
  uint16_t _pad1;

  /* RB3 Phase 3: shader bytecode for translation */
  dx9mt_upload_ref vs_bytecode;
  uint32_t vs_bytecode_dwords;
  dx9mt_upload_ref ps_bytecode;
  uint32_t ps_bytecode_dwords;

  /* RB4: depth/stencil render states */
  uint32_t rs_zenable;
  uint32_t rs_zwriteenable;
  uint32_t rs_zfunc;
  uint32_t rs_stencilenable;
  uint32_t rs_stencilfunc;
  uint32_t rs_stencilref;
  uint32_t rs_stencilmask;
  uint32_t rs_stencilwritemask;

  /* RB5: rasterizer state */
  uint32_t rs_cull_mode;
} dx9mt_packet_draw_indexed;

typedef struct dx9mt_packet_present {
  dx9mt_packet_header header;
  uint32_t frame_id;
  uint32_t flags;
  uint32_t render_target_id;
} dx9mt_packet_present;

typedef struct dx9mt_packet_clear {
  dx9mt_packet_header header;
  uint32_t frame_id;
  uint32_t rect_count;
  uint32_t flags;
  uint32_t color;
  float z;
  uint32_t stencil;
} dx9mt_packet_clear;

/*
 * header.size is uint16_t, so every packet struct must fit in 65535 bytes.
 * The (uint16_t)sizeof(packet) casts in the frontend silently truncate if
 * a packet grows past this limit. Catch it at compile time instead.
 */
_Static_assert(sizeof(dx9mt_packet_init) <= UINT16_MAX,
               "packet_init exceeds uint16 size field");
_Static_assert(sizeof(dx9mt_packet_begin_frame) <= UINT16_MAX,
               "packet_begin_frame exceeds uint16 size field");
_Static_assert(sizeof(dx9mt_packet_draw_indexed) <= UINT16_MAX,
               "packet_draw_indexed exceeds uint16 size field");
_Static_assert(sizeof(dx9mt_packet_present) <= UINT16_MAX,
               "packet_present exceeds uint16 size field");
_Static_assert(sizeof(dx9mt_packet_clear) <= UINT16_MAX,
               "packet_clear exceeds uint16 size field");

#endif
