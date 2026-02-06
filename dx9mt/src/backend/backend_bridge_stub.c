#include "dx9mt/backend_bridge.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "dx9mt/log.h"

static int g_backend_ready;
static uint32_t g_last_frame_id;
static uint32_t g_frame_packet_count;
static uint32_t g_frame_draw_indexed_count;
static uint32_t g_frame_clear_count;
static uint32_t g_last_clear_color;
static uint32_t g_last_clear_flags;
static float g_last_clear_z;
static uint32_t g_last_clear_stencil;
static int g_trace_packets = -1;

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
  case DX9MT_PACKET_CLEAR:
    return "CLEAR";
  default:
    return "UNKNOWN";
  }
}

static int dx9mt_backend_trace_packets_enabled(void) {
  const char *value;

  if (g_trace_packets >= 0) {
    return g_trace_packets;
  }

  value = getenv("DX9MT_BACKEND_TRACE_PACKETS");
  if (!value || !*value || strcmp(value, "0") == 0 ||
      strcmp(value, "false") == 0 || strcmp(value, "FALSE") == 0) {
    g_trace_packets = 0;
  } else {
    g_trace_packets = 1;
  }

  return g_trace_packets;
}

static int dx9mt_backend_should_log_frame(uint32_t frame_id) {
  return frame_id < 10 || (frame_id % 120) == 0;
}

static void dx9mt_backend_reset_frame_stats(void) {
  g_frame_packet_count = 0;
  g_frame_draw_indexed_count = 0;
  g_frame_clear_count = 0;
  g_last_clear_color = 0;
  g_last_clear_flags = 0;
  g_last_clear_z = 1.0f;
  g_last_clear_stencil = 0;
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
  dx9mt_backend_reset_frame_stats();
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
    ++g_frame_packet_count;

    if (header->type == DX9MT_PACKET_DRAW_INDEXED) {
      ++g_frame_draw_indexed_count;
    } else if (header->type == DX9MT_PACKET_CLEAR) {
      const dx9mt_packet_clear *clear_packet =
          (const dx9mt_packet_clear *)header;
      if (header->size < sizeof(*clear_packet)) {
        dx9mt_logf("backend",
                   "clear packet too small: size=%u expected=%u",
                   header->size, (unsigned)sizeof(*clear_packet));
        return -1;
      }
      ++g_frame_clear_count;
      g_last_clear_color = clear_packet->color;
      g_last_clear_flags = clear_packet->flags;
      g_last_clear_z = clear_packet->z;
      g_last_clear_stencil = clear_packet->stencil;
    }

    if (dx9mt_backend_trace_packets_enabled()) {
      dx9mt_logf("backend", "packet #%u type=%s(%u) size=%u seq=%u",
                 packet_count, dx9mt_packet_type_name(header->type), header->type,
                 header->size, header->sequence);
    }

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
  dx9mt_backend_reset_frame_stats();

  if (dx9mt_backend_should_log_frame(frame_id)) {
    dx9mt_logf("backend", "begin_frame=%u", frame_id);
  }
  return 0;
}

int dx9mt_backend_bridge_present(uint32_t frame_id) {
  if (!g_backend_ready) {
    return -1;
  }

  g_last_frame_id = frame_id;
  if (dx9mt_backend_should_log_frame(frame_id)) {
    dx9mt_logf(
        "backend",
        "present frame=%u (no-op) packets=%u draws=%u clears=%u last_clear=0x%08x flags=0x%08x z=%.3f stencil=%u",
        frame_id, g_frame_packet_count, g_frame_draw_indexed_count,
        g_frame_clear_count, g_last_clear_color, g_last_clear_flags,
        (double)g_last_clear_z, g_last_clear_stencil);
  }
  return 0;
}

void dx9mt_backend_bridge_shutdown(void) {
  if (!g_backend_ready) {
    return;
  }

  dx9mt_logf("backend", "shutdown, last_frame=%u", g_last_frame_id);
  g_backend_ready = 0;
}
