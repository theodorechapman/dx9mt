#include "dx9mt/runtime.h"

#include <string.h>

#include <windows.h>

#include "dx9mt/backend_bridge.h"
#include "dx9mt/log.h"
#include "dx9mt/packets.h"
#include "dx9mt/upload_arena.h"

static LONG g_runtime_state;
static LONG g_packet_seq;

uint32_t dx9mt_runtime_next_packet_sequence(void) {
  return (uint32_t)InterlockedIncrement(&g_packet_seq);
}

void dx9mt_runtime_ensure_initialized(void) {
  LONG previous = InterlockedCompareExchange(&g_runtime_state, 1, 0);

  if (previous == 2) {
    return;
  }

  if (previous == 1) {
    while (InterlockedCompareExchange(&g_runtime_state, 2, 2) != 2) {
      Sleep(0);
    }
    return;
  }

  dx9mt_log_init();
  dx9mt_logf("runtime", "initializing frontend/backend bridge");

  dx9mt_backend_init_desc init_desc;
  memset(&init_desc, 0, sizeof(init_desc));
  init_desc.protocol_version = 1;
  init_desc.ring_capacity_bytes = 1u << 20;
  init_desc.upload_desc.slot_count = DX9MT_UPLOAD_ARENA_SLOTS;
  init_desc.upload_desc.bytes_per_slot = DX9MT_UPLOAD_ARENA_BYTES_PER_SLOT;

  if (dx9mt_backend_bridge_init(&init_desc) == 0) {
    dx9mt_packet_init packet;
    memset(&packet, 0, sizeof(packet));
    packet.header.type = DX9MT_PACKET_INIT;
    packet.header.size = (uint16_t)sizeof(packet);
    packet.header.sequence = dx9mt_runtime_next_packet_sequence();
    packet.protocol_version = init_desc.protocol_version;
    packet.ring_capacity_bytes = init_desc.ring_capacity_bytes;
    packet.upload_desc = init_desc.upload_desc;

    dx9mt_backend_bridge_submit_packets(&packet.header, (uint32_t)sizeof(packet));
  }

  InterlockedExchange(&g_runtime_state, 2);
}

void dx9mt_runtime_shutdown(void) {
  if (InterlockedCompareExchange(&g_runtime_state, 2, 2) != 2) {
    return;
  }

  dx9mt_backend_bridge_shutdown();
  dx9mt_log_shutdown();
  InterlockedExchange(&g_packet_seq, 0);
  InterlockedExchange(&g_runtime_state, 0);
}
