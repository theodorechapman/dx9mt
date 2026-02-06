#include "dx9mt/backend_bridge.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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
static uint32_t g_last_packet_sequence;
static int g_have_present_target;
static dx9mt_backend_present_target_desc g_present_target;
static int g_frame_open;
static int g_trace_packets = -1;
static int g_soft_present = -1;

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

 #ifdef _WIN32
static int dx9mt_backend_soft_present_enabled(void) {
  const char *value;

  if (g_soft_present >= 0) {
    return g_soft_present;
  }

  value = getenv("DX9MT_BACKEND_SOFT_PRESENT");
  if (!value || !*value || strcmp(value, "0") == 0 ||
      strcmp(value, "false") == 0 || strcmp(value, "FALSE") == 0 ||
      strcmp(value, "off") == 0 || strcmp(value, "OFF") == 0 ||
      strcmp(value, "no") == 0 || strcmp(value, "NO") == 0) {
    g_soft_present = 0;
  } else {
    g_soft_present = 1;
  }

  return g_soft_present;
}
#endif

#ifdef _WIN32
static COLORREF dx9mt_colorref_from_d3dcolor(uint32_t color) {
  return RGB((color >> 16) & 0xffu, (color >> 8) & 0xffu, color & 0xffu);
}

static int dx9mt_backend_soft_present_to_window(uint32_t frame_id) {
  HWND hwnd;
  HDC hdc;
  RECT client;
  RECT marker;
  HBRUSH clear_brush;
  HBRUSH marker_brush;
  COLORREF clear_color;
  COLORREF marker_color;

  if (!dx9mt_backend_soft_present_enabled()) {
    return 0;
  }
  if (g_present_target.window_handle == 0) {
    return 0;
  }

  hwnd = (HWND)(uintptr_t)g_present_target.window_handle;
  if (!IsWindow(hwnd)) {
    return 0;
  }

  hdc = GetDC(hwnd);
  if (!hdc) {
    return 0;
  }
  if (!GetClientRect(hwnd, &client)) {
    ReleaseDC(hwnd, hdc);
    return 0;
  }

  clear_color = dx9mt_colorref_from_d3dcolor(g_last_clear_color);
  clear_brush = CreateSolidBrush(clear_color);
  if (clear_brush) {
    FillRect(hdc, &client, clear_brush);
    DeleteObject((HGDIOBJ)clear_brush);
  }

  marker = client;
  if (marker.right > marker.left + 96) {
    marker.right = marker.left + 96;
  }
  if (marker.bottom > marker.top + 16) {
    marker.bottom = marker.top + 16;
  }
  marker_color = RGB((frame_id * 13u) & 0xffu, (frame_id * 29u) & 0xffu,
                     (frame_id * 47u) & 0xffu);
  marker_brush = CreateSolidBrush(marker_color);
  if (marker_brush) {
    FillRect(hdc, &marker, marker_brush);
    DeleteObject((HGDIOBJ)marker_brush);
  }

  ReleaseDC(hwnd, hdc);
  return 1;
}
#else
static int dx9mt_backend_soft_present_to_window(uint32_t frame_id) {
  (void)frame_id;
  return 0;
}
#endif

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
  g_last_packet_sequence = 0;
  g_have_present_target = 0;
  memset(&g_present_target, 0, sizeof(g_present_target));
  g_frame_open = 0;
  g_soft_present = -1;
  dx9mt_backend_reset_frame_stats();
  return 0;
}

int dx9mt_backend_bridge_update_present_target(
    const dx9mt_backend_present_target_desc *desc) {
  if (!g_backend_ready) {
    dx9mt_logf("backend", "update_present_target called before init");
    return -1;
  }
  if (!desc || desc->width == 0 || desc->height == 0 || desc->target_id == 0) {
    dx9mt_logf(
        "backend",
        "invalid present target metadata: target=%llu size=%ux%u fmt=%u windowed=%u",
        desc ? (unsigned long long)desc->target_id : 0ull, desc ? desc->width : 0u,
        desc ? desc->height : 0u, desc ? desc->format : 0u,
        desc ? desc->windowed : 0u);
    return -1;
  }

  g_present_target = *desc;
  g_have_present_target = 1;

  dx9mt_logf(
      "backend",
      "present target updated: target=%llu hwnd=0x%llx size=%ux%u fmt=%u windowed=%u",
      (unsigned long long)g_present_target.target_id,
      (unsigned long long)g_present_target.window_handle, g_present_target.width,
      g_present_target.height, g_present_target.format, g_present_target.windowed);
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
    if (header->type <= DX9MT_PACKET_INVALID ||
        header->type > DX9MT_PACKET_CLEAR) {
      dx9mt_logf("backend", "unsupported packet type=%u size=%u seq=%u",
                 header->type, header->size, header->sequence);
      return -1;
    }
    if (header->sequence == 0 ||
        (g_last_packet_sequence != 0 &&
         header->sequence <= g_last_packet_sequence)) {
      dx9mt_logf(
          "backend",
          "packet sequence out of order: current=%u last=%u type=%u size=%u",
          header->sequence, g_last_packet_sequence, header->type, header->size);
      return -1;
    }
    g_last_packet_sequence = header->sequence;

    ++packet_count;
    ++g_frame_packet_count;

    if (header->type == DX9MT_PACKET_DRAW_INDEXED) {
      const dx9mt_packet_draw_indexed *draw_packet =
          (const dx9mt_packet_draw_indexed *)header;
      if (header->size < sizeof(*draw_packet)) {
        dx9mt_logf("backend", "draw packet too small: size=%u expected=%u",
                   header->size, (unsigned)sizeof(*draw_packet));
        return -1;
      }
      if (draw_packet->render_target_id == 0 ||
          draw_packet->vertex_buffer_id == 0 ||
          draw_packet->index_buffer_id == 0 ||
          (draw_packet->vertex_decl_id == 0 && draw_packet->fvf == 0)) {
        dx9mt_logf(
            "backend",
            "draw packet missing state ids: rt=%u vb=%u ib=%u decl=%u fvf=0x%08x seq=%u",
            draw_packet->render_target_id, draw_packet->vertex_buffer_id,
            draw_packet->index_buffer_id, draw_packet->vertex_decl_id,
            draw_packet->fvf, header->sequence);
        return -1;
      }
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

  if (g_frame_open && frame_id != g_last_frame_id) {
    dx9mt_logf("backend",
               "begin_frame out of order: incoming=%u previous_open=%u",
               frame_id, g_last_frame_id);
  }
  g_frame_open = 1;
  g_last_frame_id = frame_id;
  dx9mt_backend_reset_frame_stats();

  if (dx9mt_backend_should_log_frame(frame_id)) {
    dx9mt_logf("backend", "begin_frame=%u", frame_id);
  }
  return 0;
}

int dx9mt_backend_bridge_present(uint32_t frame_id) {
  int soft_presented;
  const char *present_mode = "no-op";

  if (!g_backend_ready) {
    return -1;
  }
  if (!g_have_present_target) {
    dx9mt_logf("backend", "present frame=%u without present-target metadata",
               frame_id);
    return -1;
  }
  if (!g_frame_open) {
    dx9mt_logf("backend", "present frame=%u without begin_frame", frame_id);
  }

  g_frame_open = 0;
  g_last_frame_id = frame_id;
  soft_presented = dx9mt_backend_soft_present_to_window(frame_id);
  if (soft_presented) {
    present_mode = "soft-present";
  }
  if (dx9mt_backend_should_log_frame(frame_id)) {
    dx9mt_logf(
        "backend",
        "present frame=%u target=%llu size=%ux%u fmt=%u (%s) packets=%u draws=%u clears=%u last_clear=0x%08x flags=0x%08x z=%.3f stencil=%u",
        frame_id, (unsigned long long)g_present_target.target_id,
        g_present_target.width, g_present_target.height, g_present_target.format,
        present_mode,
        g_frame_packet_count, g_frame_draw_indexed_count,
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
  g_have_present_target = 0;
  g_last_packet_sequence = 0;
  g_frame_open = 0;
}
