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
  dx9mt_upload_ref constants_vs;
  dx9mt_upload_ref constants_ps;
} dx9mt_packet_draw_indexed;

typedef struct dx9mt_packet_present {
  dx9mt_packet_header header;
  uint32_t frame_id;
  uint32_t flags;
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

#endif
