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
#include "dx9mt/metal_ipc.h"

#if defined(__APPLE__) && !defined(DX9MT_NO_METAL)
#include "metal_presenter.h"
#endif

/*
 * Shared-memory IPC for PE DLL -> native Metal viewer.
 * Under Wine (_WIN32), the PE DLL maps a file and writes frame data
 * that the standalone native viewer process reads and renders.
 */
#ifdef _WIN32
static HANDLE g_metal_ipc_file = INVALID_HANDLE_VALUE;
static HANDLE g_metal_ipc_mapping = NULL;
static dx9mt_metal_frame_data *g_metal_ipc_ptr = NULL;
static uint32_t g_metal_ipc_sequence = 0;
#endif

static int g_backend_ready;
static uint32_t g_last_frame_id;
static uint32_t g_frame_packet_count;
static uint32_t g_frame_draw_indexed_count;
static uint32_t g_frame_clear_count;
static uint32_t g_last_clear_color;
static uint32_t g_last_clear_flags;
static float g_last_clear_z;
static uint32_t g_last_clear_stencil;
static uint32_t g_last_draw_state_hash;
static uint32_t g_last_draw_primitive_type;
static uint32_t g_last_draw_primitive_count;
static uint32_t g_last_packet_sequence;
static int g_have_present_target;
static dx9mt_backend_present_target_desc g_present_target;
static int g_frame_open;
static int g_trace_packets = -1;
static int g_soft_present = -1;
static int g_metal_present = -1;
static dx9mt_upload_arena_desc g_upload_desc;
static uint32_t g_last_replay_hash;

typedef struct dx9mt_backend_frame_snapshot {
  uint32_t frame_id;
  uint32_t packet_count;
  uint32_t draw_count;
  uint32_t clear_count;
  uint32_t last_clear_color;
  uint32_t last_clear_flags;
  float last_clear_z;
  uint32_t last_clear_stencil;
  uint32_t last_draw_state_hash;
  uint32_t last_draw_primitive_type;
  uint32_t last_draw_primitive_count;
  uint32_t replay_hash;
  uint32_t replay_draw_count;
} dx9mt_backend_frame_snapshot;

typedef struct dx9mt_backend_draw_command {
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

  /* RB5: multi-texture stage arrays (stages 0..7) */
  uint32_t tex_id[DX9MT_MAX_PS_SAMPLERS];
  uint32_t tex_generation[DX9MT_MAX_PS_SAMPLERS];
  uint32_t tex_format[DX9MT_MAX_PS_SAMPLERS];
  uint32_t tex_width[DX9MT_MAX_PS_SAMPLERS];
  uint32_t tex_height[DX9MT_MAX_PS_SAMPLERS];
  uint32_t tex_pitch[DX9MT_MAX_PS_SAMPLERS];
  dx9mt_upload_ref tex_data[DX9MT_MAX_PS_SAMPLERS];
  uint32_t sampler_min_filter[DX9MT_MAX_PS_SAMPLERS];
  uint32_t sampler_mag_filter[DX9MT_MAX_PS_SAMPLERS];
  uint32_t sampler_mip_filter[DX9MT_MAX_PS_SAMPLERS];
  uint32_t sampler_address_u[DX9MT_MAX_PS_SAMPLERS];
  uint32_t sampler_address_v[DX9MT_MAX_PS_SAMPLERS];
  uint32_t sampler_address_w[DX9MT_MAX_PS_SAMPLERS];

  uint32_t tss0_color_op;
  uint32_t tss0_color_arg1;
  uint32_t tss0_color_arg2;
  uint32_t tss0_alpha_op;
  uint32_t tss0_alpha_arg1;
  uint32_t tss0_alpha_arg2;
  uint32_t rs_texture_factor;
  uint32_t rs_alpha_blend_enable;
  uint32_t rs_src_blend;
  uint32_t rs_dest_blend;
  uint32_t rs_alpha_test_enable;
  uint32_t rs_alpha_ref;
  uint32_t rs_alpha_func;
  dx9mt_upload_ref constants_vs;
  dx9mt_upload_ref constants_ps;

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

  dx9mt_upload_ref vertex_data;
  uint32_t vertex_data_size;
  dx9mt_upload_ref index_data;
  uint32_t index_data_size;
  uint32_t index_format;
  dx9mt_upload_ref vertex_decl_data;
  uint16_t vertex_decl_count;
  uint16_t _pad1;

  /* RB3 Phase 3: shader bytecode */
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
} dx9mt_backend_draw_command;

#define DX9MT_BACKEND_MAX_DRAW_COMMANDS_PER_FRAME 8192u

typedef struct dx9mt_backend_frame_replay_state {
  uint32_t frame_id;
  uint32_t draw_total;
  uint32_t draw_stored;
  uint32_t draw_dropped;
  int have_clear;
  dx9mt_packet_clear last_clear_packet;
  int have_present_packet;
  uint32_t present_packet_frame_id;
  uint32_t present_render_target_id;
  dx9mt_backend_draw_command
      draws[DX9MT_BACKEND_MAX_DRAW_COMMANDS_PER_FRAME];
} dx9mt_backend_frame_replay_state;

static dx9mt_backend_frame_snapshot g_current_frame_snapshot;
static dx9mt_backend_frame_snapshot g_last_presented_snapshot;
static dx9mt_backend_frame_replay_state g_frame_replay_state;

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

static uint32_t dx9mt_backend_hash_u32(uint32_t hash, uint32_t value) {
  hash ^= value;
  hash *= 16777619u;
  return hash;
}

static uint32_t dx9mt_backend_hash_upload_ref(uint32_t hash,
                                              const dx9mt_upload_ref *ref) {
  if (!ref) {
    return dx9mt_backend_hash_u32(hash, 0);
  }

  hash = dx9mt_backend_hash_u32(hash, (uint32_t)ref->arena_index);
  hash = dx9mt_backend_hash_u32(hash, ref->offset);
  hash = dx9mt_backend_hash_u32(hash, ref->size);
  return hash;
}

static uint32_t
dx9mt_backend_draw_command_hash(const dx9mt_backend_draw_command *command) {
  uint32_t hash = 2166136261u;

  if (!command) {
    return 0;
  }

  hash = dx9mt_backend_hash_u32(hash, command->state_block_hash);
  hash = dx9mt_backend_hash_u32(hash, command->primitive_type);
  hash = dx9mt_backend_hash_u32(hash, (uint32_t)command->base_vertex);
  hash = dx9mt_backend_hash_u32(hash, command->min_vertex_index);
  hash = dx9mt_backend_hash_u32(hash, command->num_vertices);
  hash = dx9mt_backend_hash_u32(hash, command->start_index);
  hash = dx9mt_backend_hash_u32(hash, command->primitive_count);
  hash = dx9mt_backend_hash_u32(hash, command->render_target_id);
  hash = dx9mt_backend_hash_u32(hash, command->depth_stencil_id);
  hash = dx9mt_backend_hash_u32(hash, command->render_target_texture_id);
  hash = dx9mt_backend_hash_u32(hash, command->render_target_width);
  hash = dx9mt_backend_hash_u32(hash, command->render_target_height);
  hash = dx9mt_backend_hash_u32(hash, command->render_target_format);
  hash = dx9mt_backend_hash_u32(hash, command->vertex_buffer_id);
  hash = dx9mt_backend_hash_u32(hash, command->index_buffer_id);
  hash = dx9mt_backend_hash_u32(hash, command->vertex_decl_id);
  hash = dx9mt_backend_hash_u32(hash, command->vertex_shader_id);
  hash = dx9mt_backend_hash_u32(hash, command->pixel_shader_id);
  hash = dx9mt_backend_hash_u32(hash, command->fvf);
  hash = dx9mt_backend_hash_u32(hash, command->stream0_offset);
  hash = dx9mt_backend_hash_u32(hash, command->stream0_stride);
  hash = dx9mt_backend_hash_u32(hash, command->viewport_hash);
  hash = dx9mt_backend_hash_u32(hash, command->scissor_hash);
  hash = dx9mt_backend_hash_u32(hash, command->texture_stage_hash);
  hash = dx9mt_backend_hash_u32(hash, command->sampler_state_hash);
  hash = dx9mt_backend_hash_u32(hash, command->stream_binding_hash);
  for (uint32_t s = 0; s < DX9MT_MAX_PS_SAMPLERS; ++s) {
    hash = dx9mt_backend_hash_u32(hash, command->tex_id[s]);
    hash = dx9mt_backend_hash_u32(hash, command->tex_generation[s]);
    hash = dx9mt_backend_hash_u32(hash, command->tex_format[s]);
    hash = dx9mt_backend_hash_u32(hash, command->tex_width[s]);
    hash = dx9mt_backend_hash_u32(hash, command->tex_height[s]);
    hash = dx9mt_backend_hash_u32(hash, command->tex_pitch[s]);
    hash = dx9mt_backend_hash_upload_ref(hash, &command->tex_data[s]);
    hash = dx9mt_backend_hash_u32(hash, command->sampler_min_filter[s]);
    hash = dx9mt_backend_hash_u32(hash, command->sampler_mag_filter[s]);
    hash = dx9mt_backend_hash_u32(hash, command->sampler_mip_filter[s]);
    hash = dx9mt_backend_hash_u32(hash, command->sampler_address_u[s]);
    hash = dx9mt_backend_hash_u32(hash, command->sampler_address_v[s]);
    hash = dx9mt_backend_hash_u32(hash, command->sampler_address_w[s]);
  }
  hash = dx9mt_backend_hash_u32(hash, command->tss0_color_op);
  hash = dx9mt_backend_hash_u32(hash, command->tss0_color_arg1);
  hash = dx9mt_backend_hash_u32(hash, command->tss0_color_arg2);
  hash = dx9mt_backend_hash_u32(hash, command->tss0_alpha_op);
  hash = dx9mt_backend_hash_u32(hash, command->tss0_alpha_arg1);
  hash = dx9mt_backend_hash_u32(hash, command->tss0_alpha_arg2);
  hash = dx9mt_backend_hash_u32(hash, command->rs_texture_factor);
  hash = dx9mt_backend_hash_u32(hash, command->rs_alpha_blend_enable);
  hash = dx9mt_backend_hash_u32(hash, command->rs_src_blend);
  hash = dx9mt_backend_hash_u32(hash, command->rs_dest_blend);
  hash = dx9mt_backend_hash_u32(hash, command->rs_alpha_test_enable);
  hash = dx9mt_backend_hash_u32(hash, command->rs_alpha_ref);
  hash = dx9mt_backend_hash_u32(hash, command->rs_alpha_func);
  hash = dx9mt_backend_hash_u32(hash, command->rs_zenable);
  hash = dx9mt_backend_hash_u32(hash, command->rs_zwriteenable);
  hash = dx9mt_backend_hash_u32(hash, command->rs_zfunc);
  hash = dx9mt_backend_hash_u32(hash, command->rs_stencilenable);
  hash = dx9mt_backend_hash_u32(hash, command->rs_stencilfunc);
  hash = dx9mt_backend_hash_u32(hash, command->rs_stencilref);
  hash = dx9mt_backend_hash_u32(hash, command->rs_stencilmask);
  hash = dx9mt_backend_hash_u32(hash, command->rs_stencilwritemask);
  hash = dx9mt_backend_hash_u32(hash, command->rs_cull_mode);
  hash = dx9mt_backend_hash_upload_ref(hash, &command->constants_vs);
  hash = dx9mt_backend_hash_upload_ref(hash, &command->constants_ps);
  return hash;
}

static uint32_t dx9mt_backend_compute_frame_replay_hash(
    const dx9mt_backend_frame_replay_state *state) {
  uint32_t hash = 2166136261u;
  uint32_t i;

  if (!state) {
    return 0;
  }

  hash = dx9mt_backend_hash_u32(hash, state->frame_id);
  hash = dx9mt_backend_hash_u32(hash, state->draw_total);
  hash = dx9mt_backend_hash_u32(hash, state->draw_stored);
  hash = dx9mt_backend_hash_u32(hash, state->draw_dropped);
  for (i = 0; i < state->draw_stored; ++i) {
    hash = dx9mt_backend_hash_u32(
        hash, dx9mt_backend_draw_command_hash(&state->draws[i]));
  }
  return hash;
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

#if defined(__APPLE__) && !defined(DX9MT_NO_METAL)
static int dx9mt_backend_metal_present_enabled(void) {
  const char *value;

  if (g_metal_present >= 0) {
    return g_metal_present;
  }

  /* Metal present defaults ON (unlike soft-present which defaults OFF). */
  value = getenv("DX9MT_BACKEND_METAL_PRESENT");
  if (value && (*value == '0' || strcmp(value, "false") == 0 ||
                strcmp(value, "FALSE") == 0 || strcmp(value, "off") == 0 ||
                strcmp(value, "OFF") == 0 || strcmp(value, "no") == 0 ||
                strcmp(value, "NO") == 0)) {
    g_metal_present = 0;
  } else {
    g_metal_present = 1;
  }

  return g_metal_present;
}
#endif

#ifdef _WIN32
static COLORREF dx9mt_colorref_from_d3dcolor(uint32_t color) {
  return RGB((color >> 16) & 0xffu, (color >> 8) & 0xffu, color & 0xffu);
}

static void dx9mt_backend_soft_present_draw_replay_preview(
    HDC hdc, const RECT *client, uint32_t frame_id) {
  enum {
    max_cells = 64,
    cells_per_row = 8,
  };
  uint32_t draw_count;
  uint32_t cells_to_draw;
  uint32_t i;

  if (!hdc || !client) {
    return;
  }

  draw_count = g_frame_replay_state.draw_stored;
  if (draw_count == 0) {
    return;
  }

  cells_to_draw = draw_count;
  if (cells_to_draw > max_cells) {
    cells_to_draw = max_cells;
  }

  for (i = 0; i < cells_to_draw; ++i) {
    RECT cell;
    uint32_t row = i / cells_per_row;
    uint32_t col = i % cells_per_row;
    uint32_t draw_hash =
        dx9mt_backend_draw_command_hash(&g_frame_replay_state.draws[i]);
    COLORREF color = RGB((draw_hash >> 16) & 0xffu, (draw_hash >> 8) & 0xffu,
                         (draw_hash ^ frame_id) & 0xffu);
    HBRUSH brush;

    cell.left = client->left + (LONG)col * 12;
    cell.top = client->top + 24 + (LONG)row * 12;
    cell.right = cell.left + 10;
    cell.bottom = cell.top + 10;
    if (cell.left >= client->right || cell.top >= client->bottom) {
      continue;
    }
    if (cell.right > client->right) {
      cell.right = client->right;
    }
    if (cell.bottom > client->bottom) {
      cell.bottom = client->bottom;
    }

    brush = CreateSolidBrush(color);
    if (brush) {
      FillRect(hdc, &cell, brush);
      DeleteObject((HGDIOBJ)brush);
    }
  }
}

static int dx9mt_backend_soft_present_to_window(
    const dx9mt_backend_frame_snapshot *snapshot) {
  HWND hwnd;
  HDC hdc;
  RECT client;
  RECT marker;
  RECT draw_bar;
  HBRUSH clear_brush;
  HBRUSH marker_brush;
  HBRUSH draw_brush;
  COLORREF clear_color;
  COLORREF marker_color;
  COLORREF draw_color;
  uint32_t frame_id;
  uint32_t draw_bar_width;

  if (!snapshot || !dx9mt_backend_soft_present_enabled()) {
    return 0;
  }
  if (g_present_target.window_handle == 0) {
    return 0;
  }
  frame_id = snapshot->frame_id;

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

  clear_color = dx9mt_colorref_from_d3dcolor(snapshot->last_clear_color);
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

  draw_bar = client;
  draw_bar.left = marker.left;
  draw_bar.top = marker.bottom;
  if (draw_bar.top < draw_bar.bottom) {
    draw_bar.bottom = draw_bar.top + 4;
    if (draw_bar.bottom > client.bottom) {
      draw_bar.bottom = client.bottom;
    }
    draw_bar_width = snapshot->draw_count;
    if (draw_bar_width > (uint32_t)(client.right - client.left)) {
      draw_bar_width = (uint32_t)(client.right - client.left);
    }
    draw_bar.right = draw_bar.left + (LONG)draw_bar_width;
    draw_color = RGB((snapshot->last_draw_state_hash >> 16) & 0xffu,
                     (snapshot->last_draw_state_hash >> 8) & 0xffu,
                     snapshot->last_draw_state_hash & 0xffu);
    draw_brush = CreateSolidBrush(draw_color);
    if (draw_brush) {
      FillRect(hdc, &draw_bar, draw_brush);
      DeleteObject((HGDIOBJ)draw_brush);
    }
  }

  dx9mt_backend_soft_present_draw_replay_preview(hdc, &client, frame_id);

  ReleaseDC(hwnd, hdc);
  return 1;
}
#else
static int dx9mt_backend_soft_present_to_window(
    const dx9mt_backend_frame_snapshot *snapshot) {
  (void)snapshot;
  return 0;
}
#endif

static int dx9mt_backend_should_log_frame(uint32_t frame_id) {
  return frame_id < 10 || (frame_id % 120) == 0;
}

static int dx9mt_backend_validate_upload_ref(const dx9mt_upload_ref *ref,
                                             const char *name,
                                             uint32_t sequence) {
  if (!ref || !name) {
    return 0;
  }
  if (ref->size == 0) {
    dx9mt_logf("backend", "draw packet missing %s payload: seq=%u", name,
               sequence);
    return 0;
  }
  if (g_upload_desc.slot_count == 0 || g_upload_desc.bytes_per_slot == 0) {
    dx9mt_logf("backend",
               "upload arena unavailable for %s: slots=%u bytes=%u seq=%u", name,
               g_upload_desc.slot_count, g_upload_desc.bytes_per_slot, sequence);
    return 0;
  }
  if ((uint32_t)ref->arena_index >= g_upload_desc.slot_count) {
    dx9mt_logf("backend",
               "upload ref arena out of range for %s: arena=%u slots=%u seq=%u",
               name, (unsigned)ref->arena_index, g_upload_desc.slot_count,
               sequence);
    return 0;
  }
  if (ref->size > g_upload_desc.bytes_per_slot ||
      ref->offset > g_upload_desc.bytes_per_slot - ref->size) {
    dx9mt_logf(
        "backend",
        "upload ref bounds invalid for %s: arena=%u offset=%u size=%u bytes=%u seq=%u",
        name, (unsigned)ref->arena_index, ref->offset, ref->size,
        g_upload_desc.bytes_per_slot, sequence);
    return 0;
  }
  return 1;
}

static void dx9mt_backend_reset_frame_stats(void) {
  g_frame_packet_count = 0;
  g_frame_draw_indexed_count = 0;
  g_frame_clear_count = 0;
  g_last_clear_color = 0;
  g_last_clear_flags = 0;
  g_last_clear_z = 1.0f;
  g_last_clear_stencil = 0;
  g_last_draw_state_hash = 0;
  g_last_draw_primitive_type = 0;
  g_last_draw_primitive_count = 0;
}

static dx9mt_backend_frame_snapshot
dx9mt_backend_capture_frame_snapshot(uint32_t frame_id) {
  dx9mt_backend_frame_snapshot snapshot;
  memset(&snapshot, 0, sizeof(snapshot));
  snapshot.frame_id = frame_id;
  snapshot.packet_count = g_frame_packet_count;
  snapshot.draw_count = g_frame_replay_state.draw_total;
  snapshot.clear_count = g_frame_clear_count;
  snapshot.last_clear_color = g_last_clear_color;
  snapshot.last_clear_flags = g_last_clear_flags;
  snapshot.last_clear_z = g_last_clear_z;
  snapshot.last_clear_stencil = g_last_clear_stencil;
  snapshot.last_draw_state_hash = g_last_draw_state_hash;
  snapshot.last_draw_primitive_type = g_last_draw_primitive_type;
  snapshot.last_draw_primitive_count = g_last_draw_primitive_count;
  snapshot.replay_hash =
      dx9mt_backend_compute_frame_replay_hash(&g_frame_replay_state);
  snapshot.replay_draw_count = g_frame_replay_state.draw_stored;
  return snapshot;
}

static void dx9mt_backend_reset_frame_replay_state(uint32_t frame_id) {
  memset(&g_frame_replay_state, 0, sizeof(g_frame_replay_state));
  g_frame_replay_state.frame_id = frame_id;
}

static void
dx9mt_backend_record_draw_command(const dx9mt_packet_draw_indexed *draw_packet) {
  dx9mt_backend_draw_command *command;

  if (!draw_packet) {
    return;
  }

  ++g_frame_replay_state.draw_total;
  if (g_frame_replay_state.draw_stored >=
      DX9MT_BACKEND_MAX_DRAW_COMMANDS_PER_FRAME) {
    ++g_frame_replay_state.draw_dropped;
    if (g_frame_replay_state.draw_dropped == 1 ||
        (g_frame_replay_state.draw_dropped % 256u) == 0u) {
      dx9mt_logf("backend",
                 "draw command capture overflow frame=%u total=%u dropped=%u",
                 g_frame_replay_state.frame_id, g_frame_replay_state.draw_total,
                 g_frame_replay_state.draw_dropped);
    }
    return;
  }

  command = &g_frame_replay_state.draws[g_frame_replay_state.draw_stored++];
  memset(command, 0, sizeof(*command));
  command->state_block_hash = draw_packet->state_block_hash;
  command->primitive_type = draw_packet->primitive_type;
  command->base_vertex = draw_packet->base_vertex;
  command->min_vertex_index = draw_packet->min_vertex_index;
  command->num_vertices = draw_packet->num_vertices;
  command->start_index = draw_packet->start_index;
  command->primitive_count = draw_packet->primitive_count;
  command->render_target_id = draw_packet->render_target_id;
  command->depth_stencil_id = draw_packet->depth_stencil_id;
  command->render_target_texture_id = draw_packet->render_target_texture_id;
  command->render_target_width = draw_packet->render_target_width;
  command->render_target_height = draw_packet->render_target_height;
  command->render_target_format = draw_packet->render_target_format;
  command->vertex_buffer_id = draw_packet->vertex_buffer_id;
  command->index_buffer_id = draw_packet->index_buffer_id;
  command->vertex_decl_id = draw_packet->vertex_decl_id;
  command->vertex_shader_id = draw_packet->vertex_shader_id;
  command->pixel_shader_id = draw_packet->pixel_shader_id;
  command->fvf = draw_packet->fvf;
  command->stream0_offset = draw_packet->stream0_offset;
  command->stream0_stride = draw_packet->stream0_stride;
  command->viewport_hash = draw_packet->viewport_hash;
  command->scissor_hash = draw_packet->scissor_hash;
  command->texture_stage_hash = draw_packet->texture_stage_hash;
  command->sampler_state_hash = draw_packet->sampler_state_hash;
  command->stream_binding_hash = draw_packet->stream_binding_hash;
  for (uint32_t s = 0; s < DX9MT_MAX_PS_SAMPLERS; ++s) {
    command->tex_id[s] = draw_packet->tex_id[s];
    command->tex_generation[s] = draw_packet->tex_generation[s];
    command->tex_format[s] = draw_packet->tex_format[s];
    command->tex_width[s] = draw_packet->tex_width[s];
    command->tex_height[s] = draw_packet->tex_height[s];
    command->tex_pitch[s] = draw_packet->tex_pitch[s];
    command->tex_data[s] = draw_packet->tex_data[s];
    command->sampler_min_filter[s] = draw_packet->sampler_min_filter[s];
    command->sampler_mag_filter[s] = draw_packet->sampler_mag_filter[s];
    command->sampler_mip_filter[s] = draw_packet->sampler_mip_filter[s];
    command->sampler_address_u[s] = draw_packet->sampler_address_u[s];
    command->sampler_address_v[s] = draw_packet->sampler_address_v[s];
    command->sampler_address_w[s] = draw_packet->sampler_address_w[s];
  }
  command->tss0_color_op = draw_packet->tss0_color_op;
  command->tss0_color_arg1 = draw_packet->tss0_color_arg1;
  command->tss0_color_arg2 = draw_packet->tss0_color_arg2;
  command->tss0_alpha_op = draw_packet->tss0_alpha_op;
  command->tss0_alpha_arg1 = draw_packet->tss0_alpha_arg1;
  command->tss0_alpha_arg2 = draw_packet->tss0_alpha_arg2;
  command->rs_texture_factor = draw_packet->rs_texture_factor;
  command->rs_alpha_blend_enable = draw_packet->rs_alpha_blend_enable;
  command->rs_src_blend = draw_packet->rs_src_blend;
  command->rs_dest_blend = draw_packet->rs_dest_blend;
  command->rs_alpha_test_enable = draw_packet->rs_alpha_test_enable;
  command->rs_alpha_ref = draw_packet->rs_alpha_ref;
  command->rs_alpha_func = draw_packet->rs_alpha_func;
  command->rs_zenable = draw_packet->rs_zenable;
  command->rs_zwriteenable = draw_packet->rs_zwriteenable;
  command->rs_zfunc = draw_packet->rs_zfunc;
  command->rs_stencilenable = draw_packet->rs_stencilenable;
  command->rs_stencilfunc = draw_packet->rs_stencilfunc;
  command->rs_stencilref = draw_packet->rs_stencilref;
  command->rs_stencilmask = draw_packet->rs_stencilmask;
  command->rs_stencilwritemask = draw_packet->rs_stencilwritemask;
  command->rs_cull_mode = draw_packet->rs_cull_mode;
  command->constants_vs = draw_packet->constants_vs;
  command->constants_ps = draw_packet->constants_ps;
  command->viewport_x = draw_packet->viewport_x;
  command->viewport_y = draw_packet->viewport_y;
  command->viewport_width = draw_packet->viewport_width;
  command->viewport_height = draw_packet->viewport_height;
  command->viewport_min_z = draw_packet->viewport_min_z;
  command->viewport_max_z = draw_packet->viewport_max_z;
  command->scissor_left = draw_packet->scissor_left;
  command->scissor_top = draw_packet->scissor_top;
  command->scissor_right = draw_packet->scissor_right;
  command->scissor_bottom = draw_packet->scissor_bottom;
  command->vertex_data = draw_packet->vertex_data;
  command->vertex_data_size = draw_packet->vertex_data_size;
  command->index_data = draw_packet->index_data;
  command->index_data_size = draw_packet->index_data_size;
  command->index_format = draw_packet->index_format;
  command->vertex_decl_data = draw_packet->vertex_decl_data;
  command->vertex_decl_count = draw_packet->vertex_decl_count;
  command->vs_bytecode = draw_packet->vs_bytecode;
  command->vs_bytecode_dwords = draw_packet->vs_bytecode_dwords;
  command->ps_bytecode = draw_packet->ps_bytecode;
  command->ps_bytecode_dwords = draw_packet->ps_bytecode_dwords;
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
  g_metal_present = -1;
  g_upload_desc = desc->upload_desc;
  g_last_replay_hash = 0;
  memset(&g_current_frame_snapshot, 0, sizeof(g_current_frame_snapshot));
  memset(&g_last_presented_snapshot, 0, sizeof(g_last_presented_snapshot));
  dx9mt_backend_reset_frame_replay_state(0);
  dx9mt_backend_reset_frame_stats();

#if defined(__APPLE__) && !defined(DX9MT_NO_METAL)
  if (dx9mt_backend_metal_present_enabled()) {
    if (dx9mt_metal_init() == 0) {
      dx9mt_logf("backend", "metal presenter initialized");
    } else {
      dx9mt_fatal("backend", "metal presenter init failed -- cannot continue");
    }
  }
#endif

#ifdef _WIN32
  /*
   * Open the shared-memory IPC file for the native Metal viewer.
   * The file is pre-created by `make run` before Wine starts.
   * If the file doesn't exist, IPC is silently disabled -- the viewer
   * wasn't launched, so there's nothing to render to.
   */
  g_metal_ipc_sequence = 0;
  g_metal_ipc_file = CreateFileA(
      DX9MT_METAL_IPC_WIN_PATH, GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  if (g_metal_ipc_file != INVALID_HANDLE_VALUE) {
    g_metal_ipc_mapping = CreateFileMappingA(g_metal_ipc_file, NULL,
                                             PAGE_READWRITE, 0,
                                             DX9MT_METAL_IPC_SIZE, NULL);
    if (g_metal_ipc_mapping) {
      g_metal_ipc_ptr = (dx9mt_metal_frame_data *)MapViewOfFile(
          g_metal_ipc_mapping, FILE_MAP_ALL_ACCESS, 0, 0,
          DX9MT_METAL_IPC_SIZE);
      if (g_metal_ipc_ptr) {
        memset((void *)g_metal_ipc_ptr, 0,
               sizeof(dx9mt_metal_ipc_global_header));
        g_metal_ipc_ptr->magic = DX9MT_METAL_IPC_MAGIC;
        g_metal_ipc_ptr->slot_size = DX9MT_METAL_IPC_SLOT_SIZE;
        dx9mt_logf("backend",
                   "metal IPC mapped at %s (double-buffered, slot_size=%u)",
                   DX9MT_METAL_IPC_WIN_PATH, DX9MT_METAL_IPC_SLOT_SIZE);
      }
    }
    if (!g_metal_ipc_ptr) {
      dx9mt_logf("backend", "metal IPC mapping failed");
      if (g_metal_ipc_mapping) {
        CloseHandle(g_metal_ipc_mapping);
        g_metal_ipc_mapping = NULL;
      }
      CloseHandle(g_metal_ipc_file);
      g_metal_ipc_file = INVALID_HANDLE_VALUE;
    }
  } else {
    dx9mt_logf("WARNING",
               "===============================================");
    dx9mt_logf("WARNING",
               "Metal IPC file not found -- viewer not running?");
    dx9mt_logf("WARNING",
               "All frame data will be DISCARDED until viewer starts.");
    dx9mt_logf("WARNING",
               "Path: %s", DX9MT_METAL_IPC_WIN_PATH);
    dx9mt_logf("WARNING",
               "===============================================");
  }
#endif

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

#if defined(__APPLE__) && !defined(DX9MT_NO_METAL)
  if (dx9mt_metal_is_available()) {
    if (dx9mt_metal_update_target(g_present_target.width,
                                  g_present_target.height,
                                  g_present_target.target_id) != 0) {
      dx9mt_logf("backend", "metal target update failed for %ux%u",
                 g_present_target.width, g_present_target.height);
    }
  }
#endif

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
      if (!dx9mt_backend_validate_upload_ref(&draw_packet->constants_vs,
                                             "constants_vs",
                                             header->sequence) ||
          !dx9mt_backend_validate_upload_ref(&draw_packet->constants_ps,
                                             "constants_ps",
                                             header->sequence)) {
        return -1;
      }
      for (uint32_t s = 0; s < DX9MT_MAX_PS_SAMPLERS; ++s) {
        if (draw_packet->tex_data[s].size > 0 &&
            !dx9mt_backend_validate_upload_ref(&draw_packet->tex_data[s],
                                               "tex_data",
                                               header->sequence)) {
          return -1;
        }
      }
      g_last_draw_state_hash = draw_packet->state_block_hash;
      g_last_draw_primitive_type = draw_packet->primitive_type;
      g_last_draw_primitive_count = draw_packet->primitive_count;
      dx9mt_backend_record_draw_command(draw_packet);
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
      g_frame_replay_state.have_clear = 1;
      g_frame_replay_state.last_clear_packet = *clear_packet;
    } else if (header->type == DX9MT_PACKET_BEGIN_FRAME) {
      /*
       * BEGIN_FRAME now arrives through the packet stream (not as a
       * side-channel direct call). Dispatch to the same begin_frame
       * logic so frame state is reset consistently whether packets
       * arrive via submit_packets or the direct API.
       */
      const dx9mt_packet_begin_frame *bf_packet =
          (const dx9mt_packet_begin_frame *)header;
      if (header->size < sizeof(*bf_packet)) {
        dx9mt_logf("backend",
                   "begin_frame packet too small: size=%u expected=%u",
                   header->size, (unsigned)sizeof(*bf_packet));
        return -1;
      }
      dx9mt_backend_bridge_begin_frame(bf_packet->frame_id);
    } else if (header->type == DX9MT_PACKET_PRESENT) {
      const dx9mt_packet_present *present_packet =
          (const dx9mt_packet_present *)header;
      if (header->size < sizeof(*present_packet)) {
        dx9mt_logf("backend",
                   "present packet too small: size=%u expected=%u",
                   header->size, (unsigned)sizeof(*present_packet));
        return -1;
      }
      g_frame_replay_state.have_present_packet = 1;
      g_frame_replay_state.present_packet_frame_id = present_packet->frame_id;
      g_frame_replay_state.present_render_target_id =
          present_packet->render_target_id;
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
  memset(&g_current_frame_snapshot, 0, sizeof(g_current_frame_snapshot));
  g_current_frame_snapshot.frame_id = frame_id;
  dx9mt_backend_reset_frame_replay_state(frame_id);

  if (dx9mt_backend_should_log_frame(frame_id)) {
    dx9mt_logf("backend", "begin_frame=%u", frame_id);
  }
  return 0;
}

int dx9mt_backend_bridge_present(uint32_t frame_id) {
  int soft_presented;
  const char *present_mode = "no-op";
  dx9mt_backend_frame_snapshot snapshot;

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
  if (g_frame_replay_state.frame_id != 0 &&
      g_frame_replay_state.frame_id != frame_id) {
    dx9mt_logf("backend",
               "present frame mismatch: incoming=%u replay_state=%u",
               frame_id, g_frame_replay_state.frame_id);
  }
  if (g_frame_replay_state.have_present_packet &&
      g_frame_replay_state.present_packet_frame_id != frame_id) {
    dx9mt_logf("backend",
               "present packet frame mismatch: packet=%u present=%u",
               g_frame_replay_state.present_packet_frame_id, frame_id);
    return -1;
  }
  if (g_frame_draw_indexed_count != g_frame_replay_state.draw_total) {
    dx9mt_logf("backend",
               "draw count mismatch: counter=%u replay_total=%u frame=%u",
               g_frame_draw_indexed_count, g_frame_replay_state.draw_total,
               frame_id);
  }

  g_frame_open = 0;
  g_last_frame_id = frame_id;
  snapshot = dx9mt_backend_capture_frame_snapshot(frame_id);
  g_current_frame_snapshot = snapshot;
  g_last_presented_snapshot = snapshot;
  g_last_replay_hash = snapshot.replay_hash;

#if defined(__APPLE__) && !defined(DX9MT_NO_METAL)
  if (dx9mt_metal_is_available()) {
    dx9mt_metal_present_desc metal_desc;
    memset(&metal_desc, 0, sizeof(metal_desc));
    metal_desc.have_clear = g_frame_replay_state.have_clear;
    metal_desc.clear_color_argb = snapshot.last_clear_color;
    metal_desc.clear_flags = snapshot.last_clear_flags;
    metal_desc.clear_z = snapshot.last_clear_z;
    metal_desc.clear_stencil = snapshot.last_clear_stencil;
    metal_desc.draw_count = g_frame_replay_state.draw_stored;
    metal_desc.replay_hash = snapshot.replay_hash;
    metal_desc.frame_id = frame_id;
    if (dx9mt_metal_present(&metal_desc) == 0) {
      present_mode = "metal";
    } else {
      present_mode = "metal-fail";
    }
  } else
#endif
  {
    soft_presented = dx9mt_backend_soft_present_to_window(&snapshot);
    if (soft_presented) {
      present_mode = "soft-present";
    }
  }

#ifdef _WIN32
  if (g_metal_ipc_ptr) {
    unsigned char *ipc_region = (unsigned char *)g_metal_ipc_ptr;
    uint32_t draw_count = g_frame_replay_state.draw_stored;
    uint32_t bulk_offset;
    uint32_t bulk_used = 0;
    dx9mt_metal_ipc_draw *ipc_draws;
    dx9mt_metal_ipc_frame_header *frame_hdr;
    uint32_t i;
    uint32_t next_seq = g_metal_ipc_sequence + 1;
    uint32_t slot_idx = next_seq % 2u;
    unsigned char *slot_base = dx9mt_ipc_slot_base(ipc_region, slot_idx);

    if (draw_count > DX9MT_METAL_IPC_MAX_DRAWS) {
      draw_count = DX9MT_METAL_IPC_MAX_DRAWS;
    }

    bulk_offset = (uint32_t)(sizeof(dx9mt_metal_ipc_frame_header) +
                             draw_count * sizeof(dx9mt_metal_ipc_draw));
    /* Align bulk data to 16 bytes */
    bulk_offset = (bulk_offset + 15u) & ~15u;

    frame_hdr = (dx9mt_metal_ipc_frame_header *)slot_base;
    ipc_draws =
        (dx9mt_metal_ipc_draw *)(slot_base +
                                 sizeof(dx9mt_metal_ipc_frame_header));

    for (i = 0; i < draw_count; ++i) {
      const dx9mt_backend_draw_command *cmd = &g_frame_replay_state.draws[i];
      dx9mt_metal_ipc_draw *d = &ipc_draws[i];
      const void *data;

      memset(d, 0, sizeof(*d));
      d->primitive_type = cmd->primitive_type;
      d->base_vertex = cmd->base_vertex;
      d->min_vertex_index = cmd->min_vertex_index;
      d->num_vertices = cmd->num_vertices;
      d->start_index = cmd->start_index;
      d->primitive_count = cmd->primitive_count;
      d->render_target_id = cmd->render_target_id;
      d->render_target_texture_id = cmd->render_target_texture_id;
      d->render_target_width = cmd->render_target_width;
      d->render_target_height = cmd->render_target_height;
      d->render_target_format = cmd->render_target_format;
      d->viewport_x = cmd->viewport_x;
      d->viewport_y = cmd->viewport_y;
      d->viewport_width = cmd->viewport_width;
      d->viewport_height = cmd->viewport_height;
      d->viewport_min_z = cmd->viewport_min_z;
      d->viewport_max_z = cmd->viewport_max_z;
      d->scissor_left = cmd->scissor_left;
      d->scissor_top = cmd->scissor_top;
      d->scissor_right = cmd->scissor_right;
      d->scissor_bottom = cmd->scissor_bottom;
      d->fvf = cmd->fvf;
      d->pixel_shader_id = cmd->pixel_shader_id;
      d->stream0_offset = cmd->stream0_offset;
      d->stream0_stride = cmd->stream0_stride;
      d->index_format = cmd->index_format;
      for (uint32_t s = 0; s < DX9MT_MAX_PS_SAMPLERS; ++s) {
        d->tex_id[s] = cmd->tex_id[s];
        d->tex_generation[s] = cmd->tex_generation[s];
        d->tex_format[s] = cmd->tex_format[s];
        d->tex_width[s] = cmd->tex_width[s];
        d->tex_height[s] = cmd->tex_height[s];
        d->tex_pitch[s] = cmd->tex_pitch[s];
        d->sampler_min_filter[s] = cmd->sampler_min_filter[s];
        d->sampler_mag_filter[s] = cmd->sampler_mag_filter[s];
        d->sampler_mip_filter[s] = cmd->sampler_mip_filter[s];
        d->sampler_address_u[s] = cmd->sampler_address_u[s];
        d->sampler_address_v[s] = cmd->sampler_address_v[s];
        d->sampler_address_w[s] = cmd->sampler_address_w[s];
      }
      d->tss0_color_op = cmd->tss0_color_op;
      d->tss0_color_arg1 = cmd->tss0_color_arg1;
      d->tss0_color_arg2 = cmd->tss0_color_arg2;
      d->tss0_alpha_op = cmd->tss0_alpha_op;
      d->tss0_alpha_arg1 = cmd->tss0_alpha_arg1;
      d->tss0_alpha_arg2 = cmd->tss0_alpha_arg2;
      d->rs_texture_factor = cmd->rs_texture_factor;
      d->rs_alpha_blend_enable = cmd->rs_alpha_blend_enable;
      d->rs_src_blend = cmd->rs_src_blend;
      d->rs_dest_blend = cmd->rs_dest_blend;
      d->rs_alpha_test_enable = cmd->rs_alpha_test_enable;
      d->rs_alpha_ref = cmd->rs_alpha_ref;
      d->rs_alpha_func = cmd->rs_alpha_func;
      d->rs_zenable = cmd->rs_zenable;
      d->rs_zwriteenable = cmd->rs_zwriteenable;
      d->rs_zfunc = cmd->rs_zfunc;
      d->rs_stencilenable = cmd->rs_stencilenable;
      d->rs_stencilfunc = cmd->rs_stencilfunc;
      d->rs_stencilref = cmd->rs_stencilref;
      d->rs_stencilmask = cmd->rs_stencilmask;
      d->rs_stencilwritemask = cmd->rs_stencilwritemask;
      d->rs_cull_mode = cmd->rs_cull_mode;

      /* Copy VB data into bulk region */
      data = dx9mt_frontend_upload_resolve(&cmd->vertex_data);
      if (data && cmd->vertex_data_size > 0 &&
          bulk_offset + bulk_used + cmd->vertex_data_size <=
              DX9MT_METAL_IPC_SLOT_SIZE) {
        d->vb_bulk_offset = bulk_used;
        d->vb_bulk_size = cmd->vertex_data_size;
        memcpy(slot_base + bulk_offset + bulk_used, data,
               cmd->vertex_data_size);
        bulk_used += (cmd->vertex_data_size + 15u) & ~15u;
      }

      /* Copy IB data into bulk region */
      data = dx9mt_frontend_upload_resolve(&cmd->index_data);
      if (data && cmd->index_data_size > 0 &&
          bulk_offset + bulk_used + cmd->index_data_size <=
              DX9MT_METAL_IPC_SLOT_SIZE) {
        d->ib_bulk_offset = bulk_used;
        d->ib_bulk_size = cmd->index_data_size;
        memcpy(slot_base + bulk_offset + bulk_used, data,
               cmd->index_data_size);
        bulk_used += (cmd->index_data_size + 15u) & ~15u;
      }

      /* Copy vertex declaration into bulk region */
      data = dx9mt_frontend_upload_resolve(&cmd->vertex_decl_data);
      if (data && cmd->vertex_decl_count > 0) {
        uint32_t decl_bytes = cmd->vertex_decl_count * 8u;
        if (bulk_offset + bulk_used + decl_bytes <=
            DX9MT_METAL_IPC_SLOT_SIZE) {
          d->decl_bulk_offset = bulk_used;
          d->decl_count = cmd->vertex_decl_count;
          memcpy(slot_base + bulk_offset + bulk_used, data, decl_bytes);
          bulk_used += (decl_bytes + 15u) & ~15u;
        }
      }

      /* Copy VS float constants into bulk region */
      data = dx9mt_frontend_upload_resolve(&cmd->constants_vs);
      if (data && cmd->constants_vs.size > 0 &&
          bulk_offset + bulk_used + cmd->constants_vs.size <=
              DX9MT_METAL_IPC_SLOT_SIZE) {
        d->vs_constants_bulk_offset = bulk_used;
        d->vs_constants_size = cmd->constants_vs.size;
        memcpy(slot_base + bulk_offset + bulk_used, data,
               cmd->constants_vs.size);
        bulk_used += (cmd->constants_vs.size + 15u) & ~15u;
      }

      /* Copy PS float constants into bulk region */
      data = dx9mt_frontend_upload_resolve(&cmd->constants_ps);
      if (data && cmd->constants_ps.size > 0 &&
          bulk_offset + bulk_used + cmd->constants_ps.size <=
              DX9MT_METAL_IPC_SLOT_SIZE) {
        d->ps_constants_bulk_offset = bulk_used;
        d->ps_constants_size = cmd->constants_ps.size;
        memcpy(slot_base + bulk_offset + bulk_used, data,
               cmd->constants_ps.size);
        bulk_used += (cmd->constants_ps.size + 15u) & ~15u;
      }

      /* Copy per-stage texture uploads when present (texture cache updates). */
      for (uint32_t s = 0; s < DX9MT_MAX_PS_SAMPLERS; ++s) {
        data = dx9mt_frontend_upload_resolve(&cmd->tex_data[s]);
        if (data && cmd->tex_data[s].size > 0 &&
            bulk_offset + bulk_used + cmd->tex_data[s].size <=
                DX9MT_METAL_IPC_SLOT_SIZE) {
          d->tex_bulk_offset[s] = bulk_used;
          d->tex_bulk_size[s] = cmd->tex_data[s].size;
          memcpy(slot_base + bulk_offset + bulk_used, data,
                 cmd->tex_data[s].size);
          bulk_used += (cmd->tex_data[s].size + 15u) & ~15u;
        }
      }

      /* Copy VS/PS shader bytecode for translation. */
      d->vertex_shader_id = cmd->vertex_shader_id;
      data = dx9mt_frontend_upload_resolve(&cmd->vs_bytecode);
      if (data && cmd->vs_bytecode.size > 0 &&
          bulk_offset + bulk_used + cmd->vs_bytecode.size <=
              DX9MT_METAL_IPC_SLOT_SIZE) {
        d->vs_bytecode_bulk_offset = bulk_used;
        d->vs_bytecode_bulk_size = cmd->vs_bytecode.size;
        memcpy(slot_base + bulk_offset + bulk_used, data,
               cmd->vs_bytecode.size);
        bulk_used += (cmd->vs_bytecode.size + 15u) & ~15u;
      }
      data = dx9mt_frontend_upload_resolve(&cmd->ps_bytecode);
      if (data && cmd->ps_bytecode.size > 0 &&
          bulk_offset + bulk_used + cmd->ps_bytecode.size <=
              DX9MT_METAL_IPC_SLOT_SIZE) {
        d->ps_bytecode_bulk_offset = bulk_used;
        d->ps_bytecode_bulk_size = cmd->ps_bytecode.size;
        memcpy(slot_base + bulk_offset + bulk_used, data,
               cmd->ps_bytecode.size);
        bulk_used += (cmd->ps_bytecode.size + 15u) & ~15u;
      }
    }

    frame_hdr->width = g_present_target.width;
    frame_hdr->height = g_present_target.height;
    frame_hdr->have_clear = g_frame_replay_state.have_clear;
    frame_hdr->clear_color_argb = snapshot.last_clear_color;
    frame_hdr->clear_flags = snapshot.last_clear_flags;
    frame_hdr->clear_z = snapshot.last_clear_z;
    frame_hdr->clear_stencil = snapshot.last_clear_stencil;
    frame_hdr->draw_count = draw_count;
    frame_hdr->replay_hash = snapshot.replay_hash;
    frame_hdr->frame_id = frame_id;
    frame_hdr->present_render_target_id =
        g_frame_replay_state.present_render_target_id;
    frame_hdr->bulk_data_offset = bulk_offset;
    frame_hdr->bulk_data_used = bulk_used;
    /*
     * Write sequence last -- the viewer polls this field.
     * The viewer reads from slot (sequence % 2), which is the slot
     * we just finished writing. Our NEXT frame will go to the other slot.
     */
    g_metal_ipc_sequence = next_seq;
    __atomic_store_n(&g_metal_ipc_ptr->sequence, next_seq,
                     __ATOMIC_RELEASE);
    present_mode = "metal-ipc";
  }
#endif
  if (dx9mt_backend_should_log_frame(frame_id)) {
    dx9mt_logf(
        "backend",
        "present frame=%u target=%llu size=%ux%u fmt=%u (%s) packets=%u draws=%u clears=%u last_clear=0x%08x flags=0x%08x z=%.3f stencil=%u draw_hash=0x%08x replay_hash=0x%08x draw_stored=%u draw_dropped=%u",
        frame_id, (unsigned long long)g_present_target.target_id,
        g_present_target.width, g_present_target.height, g_present_target.format,
        present_mode, snapshot.packet_count, snapshot.draw_count,
        snapshot.clear_count, snapshot.last_clear_color,
        snapshot.last_clear_flags, (double)snapshot.last_clear_z,
        snapshot.last_clear_stencil, snapshot.last_draw_state_hash,
        snapshot.replay_hash,
        g_frame_replay_state.draw_stored, g_frame_replay_state.draw_dropped);
  }
  g_frame_replay_state.have_present_packet = 0;
  return 0;
}

void dx9mt_backend_bridge_shutdown(void) {
  if (!g_backend_ready) {
    return;
  }

#if defined(__APPLE__) && !defined(DX9MT_NO_METAL)
  if (dx9mt_metal_is_available()) {
    dx9mt_metal_shutdown();
  }
#endif

#ifdef _WIN32
  if (g_metal_ipc_ptr) {
    UnmapViewOfFile(g_metal_ipc_ptr);
    g_metal_ipc_ptr = NULL;
  }
  if (g_metal_ipc_mapping) {
    CloseHandle(g_metal_ipc_mapping);
    g_metal_ipc_mapping = NULL;
  }
  if (g_metal_ipc_file != INVALID_HANDLE_VALUE) {
    CloseHandle(g_metal_ipc_file);
    g_metal_ipc_file = INVALID_HANDLE_VALUE;
  }
#endif

  dx9mt_logf("backend", "shutdown, last_frame=%u", g_last_frame_id);
  g_backend_ready = 0;
  g_have_present_target = 0;
  g_last_packet_sequence = 0;
  g_frame_open = 0;
  memset(&g_upload_desc, 0, sizeof(g_upload_desc));
  g_last_replay_hash = 0;
  dx9mt_backend_reset_frame_replay_state(0);
}

uint32_t dx9mt_backend_bridge_debug_get_last_replay_hash(void) {
  return g_last_replay_hash;
}
