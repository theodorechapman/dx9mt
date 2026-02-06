#include "dx9mt/backend_bridge.h"

#include <stddef.h>

#include "dx9mt/log.h"

static int g_backend_ready;
static uint32_t g_last_frame_id;

static const char *dx9mt_packet_type_name(uint16_t type) {
  switch (type) {
  case DX9MT_PACKET_INIT:
    return "INIT";
  case DX9MT_PACKET_BEGIN_FRAME:
    return "BEGIN_FRAME";
  case DX9MT_PACKET_DRAW_INDEXED:
    return "DRAW_INDEXED";
  case DX9MT_PACKET_PRESENT:
    return "PRESENT";
  case DX9MT_PACKET_SHUTDOWN:
    return "SHUTDOWN";
  default:
    return "UNKNOWN";
  }
}

int dx9mt_backend_bridge_init(const dx9mt_backend_init_desc *desc) {
  if (!desc) {
    return -1;
  }

  dx9mt_logf("backend", "bridge init: protocol=%u ring=%u upload_slots=%u upload_bytes=%u",
             desc->protocol_version, desc->ring_capacity_bytes,
             desc->upload_desc.slot_count, desc->upload_desc.bytes_per_slot);

  g_backend_ready = 1;
  g_last_frame_id = 0;
  return 0;
}

int dx9mt_backend_bridge_submit_packets(const dx9mt_packet_header *packets,
                                        uint32_t packet_bytes) {
  uint32_t offset = 0;
  uint32_t packet_count = 0;

  if (!g_backend_ready) {
    dx9mt_logf("backend", "submit_packets called before init");
    return -1;
  }

  if (!packets || packet_bytes == 0) {
    return 0;
  }

  while (offset + sizeof(dx9mt_packet_header) <= packet_bytes) {
    const dx9mt_packet_header *header;

    header = (const dx9mt_packet_header *)((const unsigned char *)packets + offset);
    if (header->size < sizeof(dx9mt_packet_header) ||
        offset + header->size > packet_bytes) {
      dx9mt_logf("backend",
                 "packet parse error: offset=%u size=%u total=%u",
                 offset, header->size, packet_bytes);
      return -1;
    }

    ++packet_count;
    dx9mt_logf("backend", "packet #%u type=%s(%u) size=%u seq=%u",
               packet_count, dx9mt_packet_type_name(header->type), header->type,
               header->size, header->sequence);

    offset += header->size;
  }

  if (offset != packet_bytes) {
    dx9mt_logf("backend", "packet tail mismatch: parsed=%u total=%u", offset,
               packet_bytes);
    return -1;
  }

  return 0;
}

int dx9mt_backend_bridge_begin_frame(uint32_t frame_id) {
  if (!g_backend_ready) {
    return -1;
  }

  g_last_frame_id = frame_id;
  dx9mt_logf("backend", "begin_frame=%u", frame_id);
  return 0;
}

int dx9mt_backend_bridge_present(uint32_t frame_id) {
  if (!g_backend_ready) {
    return -1;
  }

  g_last_frame_id = frame_id;
  dx9mt_logf("backend", "present frame=%u (no-op)", frame_id);
  return 0;
}

void dx9mt_backend_bridge_shutdown(void) {
  if (!g_backend_ready) {
    return;
  }

  dx9mt_logf("backend", "shutdown, last_frame=%u", g_last_frame_id);
  g_backend_ready = 0;
}
