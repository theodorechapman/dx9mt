#ifndef DX9MT_UPLOAD_ARENA_H
#define DX9MT_UPLOAD_ARENA_H

#include <stdint.h>

enum {
  DX9MT_UPLOAD_ARENA_SLOTS = 3,
  DX9MT_UPLOAD_ARENA_BYTES_PER_SLOT = 8u << 20,
};

/*
 * Upload ref: references a range within a triple-buffered upload slot.
 * Embedded inside dx9mt_packet_draw_indexed which crosses the PE32/ARM64
 * boundary. Explicit padding ensures layout agreement between the
 * i686-mingw frontend and clang-arm64 backend compilers -- without it,
 * the 2-byte gap after arena_index is implicitly inserted by natural
 * alignment but not guaranteed to be identical across ABIs.
 */
typedef struct dx9mt_upload_ref {
  uint16_t arena_index;
  uint16_t _pad0;
  uint32_t offset;
  uint32_t size;
} dx9mt_upload_ref;

typedef struct dx9mt_upload_arena_desc {
  uint32_t slot_count;
  uint32_t bytes_per_slot;
} dx9mt_upload_arena_desc;

/*
 * Resolve an upload ref to a read-only data pointer. Returns NULL if
 * the ref is invalid or the arena is not available. Only valid in the
 * frontend process (same address space as the upload arena).
 */
const void *dx9mt_frontend_upload_resolve(const dx9mt_upload_ref *ref);

#endif
