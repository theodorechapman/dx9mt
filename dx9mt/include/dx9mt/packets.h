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
  dx9mt_upload_ref constants_vs;
  dx9mt_upload_ref constants_ps;
} dx9mt_packet_draw_indexed;

typedef struct dx9mt_packet_present {
  dx9mt_packet_header header;
  uint32_t frame_id;
  uint32_t flags;
} dx9mt_packet_present;

#endif
