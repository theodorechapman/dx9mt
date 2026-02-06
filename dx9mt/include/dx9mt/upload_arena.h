#ifndef DX9MT_UPLOAD_ARENA_H
#define DX9MT_UPLOAD_ARENA_H

#include <stdint.h>

enum {
  DX9MT_UPLOAD_ARENA_SLOTS = 3,
};

typedef struct dx9mt_upload_ref {
  uint16_t arena_index;
  uint32_t offset;
  uint32_t size;
} dx9mt_upload_ref;

typedef struct dx9mt_upload_arena_desc {
  uint32_t slot_count;
  uint32_t bytes_per_slot;
} dx9mt_upload_arena_desc;

#endif
