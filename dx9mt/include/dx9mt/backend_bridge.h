#ifndef DX9MT_BACKEND_BRIDGE_H
#define DX9MT_BACKEND_BRIDGE_H

#include <stdint.h>

#include "dx9mt/packets.h"
#include "dx9mt/upload_arena.h"

typedef struct dx9mt_backend_init_desc {
  uint32_t protocol_version;
  uint32_t ring_capacity_bytes;
  dx9mt_upload_arena_desc upload_desc;
} dx9mt_backend_init_desc;

typedef struct dx9mt_backend_present_target_desc {
  uint64_t target_id;
  uint64_t window_handle;
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t windowed;
} dx9mt_backend_present_target_desc;

int dx9mt_backend_bridge_init(const dx9mt_backend_init_desc *desc);
int dx9mt_backend_bridge_update_present_target(
    const dx9mt_backend_present_target_desc *desc);
int dx9mt_backend_bridge_submit_packets(const dx9mt_packet_header *packets,
                                        uint32_t packet_bytes);
int dx9mt_backend_bridge_begin_frame(uint32_t frame_id);
int dx9mt_backend_bridge_present(uint32_t frame_id);
void dx9mt_backend_bridge_shutdown(void);
uint32_t dx9mt_backend_bridge_debug_get_last_replay_hash(void);

#endif
