#include "dx9mt/d3d9_device.h"

#include <windows.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "dx9mt/backend_bridge.h"
#include "dx9mt/log.h"
#include "dx9mt/object_ids.h"
#include "dx9mt/packets.h"
#include "dx9mt/runtime.h"

#define DX9MT_MAX_RENDER_TARGETS 4
#define DX9MT_MAX_TEXTURE_STAGES 16
#define DX9MT_MAX_SAMPLERS 20
#define DX9MT_MAX_SAMPLER_STATES 16
#define DX9MT_MAX_TEXTURE_STAGE_STATES 32
#define DX9MT_MAX_RENDER_STATES 256
#define DX9MT_MAX_STREAMS 16
#define DX9MT_MAX_TRANSFORM_STATES 512
#define DX9MT_MAX_SHADER_FLOAT_CONSTANTS 256
#define DX9MT_MAX_SHADER_INT_CONSTANTS 16
#define DX9MT_MAX_SHADER_BOOL_CONSTANTS 16
#define DX9MT_UPLOAD_BYTES_PER_SLOT DX9MT_UPLOAD_ARENA_BYTES_PER_SLOT
#define DX9MT_TEXTURE_UPLOAD_REFRESH_INTERVAL 60u
#define DX9MT_DRAW_SHADER_CONSTANT_BYTES                                          \
  (DX9MT_MAX_SHADER_FLOAT_CONSTANTS * 4u * sizeof(float))

#ifndef D3DFMT_DXT1
#define DX9MT_MAKEFOURCC(ch0, ch1, ch2, ch3)                                      \
  ((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) |                   \
   ((uint32_t)(uint8_t)(ch2) << 16) | ((uint32_t)(uint8_t)(ch3) << 24))
#define D3DFMT_DXT1 ((D3DFORMAT)DX9MT_MAKEFOURCC('D', 'X', 'T', '1'))
#define D3DFMT_DXT3 ((D3DFORMAT)DX9MT_MAKEFOURCC('D', 'X', 'T', '3'))
#define D3DFMT_DXT5 ((D3DFORMAT)DX9MT_MAKEFOURCC('D', 'X', 'T', '5'))
#endif

static WINBOOL dx9mt_should_log_method_sample(LONG *counter, LONG first_n,
                                              LONG every_n) {
  LONG count = InterlockedIncrement(counter);
  if (count <= first_n) {
    return TRUE;
  }
  if (every_n > 0 && (count % every_n) == 0) {
    return TRUE;
  }
  return FALSE;
}

static LONG g_object_id_counters[DX9MT_OBJECT_KIND_VERTEX_DECL + 1];

typedef struct dx9mt_frontend_upload_state {
  uint32_t frame_id;
  uint16_t slot_index;
  uint32_t next_offset;
  unsigned char slots[DX9MT_UPLOAD_ARENA_SLOTS][DX9MT_UPLOAD_BYTES_PER_SLOT];
} dx9mt_frontend_upload_state;

static dx9mt_frontend_upload_state *g_frontend_upload_state;

static dx9mt_frontend_upload_state *dx9mt_frontend_upload_ensure(void) {
  if (!g_frontend_upload_state) {
    g_frontend_upload_state = (dx9mt_frontend_upload_state *)VirtualAlloc(
        NULL, sizeof(dx9mt_frontend_upload_state), MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
    if (!g_frontend_upload_state) {
      dx9mt_logf("upload", "FATAL: VirtualAlloc failed for upload state (%u bytes)",
                 (unsigned)sizeof(dx9mt_frontend_upload_state));
    }
  }
  return g_frontend_upload_state;
}

const void *dx9mt_frontend_upload_resolve(const dx9mt_upload_ref *ref) {
  if (!ref || ref->size == 0) {
    return NULL;
  }
  if ((uint32_t)ref->arena_index >= DX9MT_UPLOAD_ARENA_SLOTS) {
    return NULL;
  }
  if (ref->offset + ref->size > DX9MT_UPLOAD_BYTES_PER_SLOT) {
    return NULL;
  }
  if (!g_frontend_upload_state) {
    return NULL;
  }
  return g_frontend_upload_state->slots[ref->arena_index] + ref->offset;
}

typedef struct dx9mt_device dx9mt_device;
typedef struct dx9mt_surface dx9mt_surface;
typedef struct dx9mt_swapchain dx9mt_swapchain;
typedef struct dx9mt_vertex_buffer dx9mt_vertex_buffer;
typedef struct dx9mt_index_buffer dx9mt_index_buffer;
typedef struct dx9mt_vertex_decl dx9mt_vertex_decl;
typedef struct dx9mt_vertex_shader dx9mt_vertex_shader;
typedef struct dx9mt_pixel_shader dx9mt_pixel_shader;
typedef struct dx9mt_texture dx9mt_texture;
typedef struct dx9mt_cube_texture dx9mt_cube_texture;
typedef struct dx9mt_query dx9mt_query;

struct dx9mt_surface {
  IDirect3DSurface9 iface;
  LONG refcount;
  dx9mt_object_id object_id;
  dx9mt_device *device;
  IUnknown *container;
  D3DSURFACE_DESC desc;
  WINBOOL lockable;
  unsigned char *sysmem;
  UINT pitch;
};

struct dx9mt_swapchain {
  IDirect3DSwapChain9 iface;
  LONG refcount;
  dx9mt_object_id object_id;
  dx9mt_device *device;
  D3DPRESENT_PARAMETERS params;
  IDirect3DSurface9 *backbuffer;
  UINT present_count;
};

struct dx9mt_vertex_buffer {
  IDirect3DVertexBuffer9 iface;
  LONG refcount;
  dx9mt_object_id object_id;
  dx9mt_device *device;
  D3DVERTEXBUFFER_DESC desc;
  unsigned char *data;
};

struct dx9mt_index_buffer {
  IDirect3DIndexBuffer9 iface;
  LONG refcount;
  dx9mt_object_id object_id;
  dx9mt_device *device;
  D3DINDEXBUFFER_DESC desc;
  unsigned char *data;
};

struct dx9mt_vertex_decl {
  IDirect3DVertexDeclaration9 iface;
  LONG refcount;
  dx9mt_object_id object_id;
  dx9mt_device *device;
  D3DVERTEXELEMENT9 *elements;
  UINT count;
};

struct dx9mt_vertex_shader {
  IDirect3DVertexShader9 iface;
  LONG refcount;
  dx9mt_object_id object_id;
  dx9mt_device *device;
  DWORD *byte_code;
  UINT dword_count;
};

struct dx9mt_pixel_shader {
  IDirect3DPixelShader9 iface;
  LONG refcount;
  dx9mt_object_id object_id;
  dx9mt_device *device;
  DWORD *byte_code;
  UINT dword_count;
};

struct dx9mt_texture {
  IDirect3DTexture9 iface;
  LONG refcount;
  dx9mt_object_id object_id;
  dx9mt_device *device;
  DWORD usage;
  D3DFORMAT format;
  D3DPOOL pool;
  UINT width;
  UINT height;
  UINT levels;
  DWORD lod;
  D3DTEXTUREFILTERTYPE autogen_filter;
  uint32_t generation;
  uint32_t last_upload_generation;
  uint32_t last_upload_frame_id;
  IDirect3DSurface9 **surfaces;
};

struct dx9mt_cube_texture {
  IDirect3DCubeTexture9 iface;
  LONG refcount;
  dx9mt_object_id object_id;
  dx9mt_device *device;
  DWORD usage;
  D3DFORMAT format;
  D3DPOOL pool;
  UINT edge_length;
  UINT levels;
  DWORD lod;
  D3DTEXTUREFILTERTYPE autogen_filter;
  uint32_t generation;
  IDirect3DSurface9 **surfaces;
};

struct dx9mt_query {
  IDirect3DQuery9 iface;
  LONG refcount;
  dx9mt_object_id object_id;
  dx9mt_device *device;
  D3DQUERYTYPE type;
  DWORD data_size;
  DWORD issue_flags;
  WINBOOL issued;
};

struct dx9mt_device {
  IDirect3DDevice9 iface;
  LONG refcount;

  IDirect3D9 *parent;
  UINT adapter;
  D3DDEVTYPE device_type;
  HWND focus_window;
  DWORD behavior_flags;

  D3DPRESENT_PARAMETERS params;
  D3DDEVICE_CREATION_PARAMETERS creation;
  D3DGAMMARAMP gamma_ramp;

  D3DVIEWPORT9 viewport;
  RECT scissor_rect;

  WINBOOL in_scene;
  WINBOOL software_vp;
  float n_patch_mode;
  DWORD fvf;
  UINT frame_id;
  uint64_t present_target_id;

  DWORD render_states[DX9MT_MAX_RENDER_STATES];
  DWORD sampler_states[DX9MT_MAX_SAMPLERS][DX9MT_MAX_SAMPLER_STATES];
  DWORD tex_stage_states[DX9MT_MAX_TEXTURE_STAGES][DX9MT_MAX_TEXTURE_STAGE_STATES];

  IDirect3DSurface9 *render_targets[DX9MT_MAX_RENDER_TARGETS];
  IDirect3DSurface9 *depth_stencil;

  IDirect3DBaseTexture9 *textures[DX9MT_MAX_TEXTURE_STAGES];

  IDirect3DVertexBuffer9 *streams[DX9MT_MAX_STREAMS];
  UINT stream_offsets[DX9MT_MAX_STREAMS];
  UINT stream_strides[DX9MT_MAX_STREAMS];
  UINT stream_freq[DX9MT_MAX_STREAMS];
  IDirect3DIndexBuffer9 *indices;

  IDirect3DVertexDeclaration9 *vertex_decl;
  IDirect3DVertexShader9 *vertex_shader;
  IDirect3DPixelShader9 *pixel_shader;

  D3DMATRIX transforms[DX9MT_MAX_TRANSFORM_STATES];
  WINBOOL transform_set[DX9MT_MAX_TRANSFORM_STATES];
  float clip_planes[6][4];

  float vs_const_f[DX9MT_MAX_SHADER_FLOAT_CONSTANTS][4];
  float ps_const_f[DX9MT_MAX_SHADER_FLOAT_CONSTANTS][4];
  int vs_const_i[DX9MT_MAX_SHADER_INT_CONSTANTS][4];
  int ps_const_i[DX9MT_MAX_SHADER_INT_CONSTANTS][4];
  WINBOOL vs_const_b[DX9MT_MAX_SHADER_BOOL_CONSTANTS];
  WINBOOL ps_const_b[DX9MT_MAX_SHADER_BOOL_CONSTANTS];

  /* Dirty tracking for shader constants -- avoid re-uploading when unchanged */
  WINBOOL vs_const_dirty;
  WINBOOL ps_const_dirty;
  dx9mt_upload_ref vs_const_last_ref;
  dx9mt_upload_ref ps_const_last_ref;

  dx9mt_swapchain *swapchain;
};

static dx9mt_device *dx9mt_device_from_iface(IDirect3DDevice9 *iface) {
  return (dx9mt_device *)iface;
}

static dx9mt_surface *dx9mt_surface_from_iface(IDirect3DSurface9 *iface) {
  return (dx9mt_surface *)iface;
}

static dx9mt_swapchain *dx9mt_swapchain_from_iface(IDirect3DSwapChain9 *iface) {
  return (dx9mt_swapchain *)iface;
}

static dx9mt_vertex_buffer *dx9mt_vb_from_iface(IDirect3DVertexBuffer9 *iface) {
  return (dx9mt_vertex_buffer *)iface;
}

static dx9mt_index_buffer *dx9mt_ib_from_iface(IDirect3DIndexBuffer9 *iface) {
  return (dx9mt_index_buffer *)iface;
}

static dx9mt_vertex_decl *dx9mt_vdecl_from_iface(IDirect3DVertexDeclaration9 *iface) {
  return (dx9mt_vertex_decl *)iface;
}

static dx9mt_vertex_shader *dx9mt_vshader_from_iface(IDirect3DVertexShader9 *iface) {
  return (dx9mt_vertex_shader *)iface;
}

static dx9mt_pixel_shader *dx9mt_pshader_from_iface(IDirect3DPixelShader9 *iface) {
  return (dx9mt_pixel_shader *)iface;
}

static dx9mt_texture *dx9mt_texture_from_iface(IDirect3DTexture9 *iface) {
  return (dx9mt_texture *)iface;
}

static dx9mt_cube_texture *dx9mt_cube_texture_from_iface(
    IDirect3DCubeTexture9 *iface) {
  return (dx9mt_cube_texture *)iface;
}

static dx9mt_query *dx9mt_query_from_iface(IDirect3DQuery9 *iface) {
  return (dx9mt_query *)iface;
}

static dx9mt_object_id dx9mt_alloc_object_id(enum dx9mt_object_kind kind) {
  LONG next;
  uint32_t serial;

  if (kind <= DX9MT_OBJECT_KIND_INVALID ||
      kind > DX9MT_OBJECT_KIND_VERTEX_DECL) {
    return 0;
  }

  next = InterlockedIncrement(&g_object_id_counters[kind]);
  serial = (uint32_t)next & 0x00FFFFFFu;
  if (serial == 0) {
    serial = 1;
  }

  return ((uint32_t)kind << 24) | serial;
}

static dx9mt_object_id
dx9mt_surface_object_id_from_iface(IDirect3DSurface9 *iface) {
  if (!iface) {
    return 0;
  }
  return dx9mt_surface_from_iface(iface)->object_id;
}

static dx9mt_object_id
dx9mt_vb_object_id_from_iface(IDirect3DVertexBuffer9 *iface) {
  if (!iface) {
    return 0;
  }
  return dx9mt_vb_from_iface(iface)->object_id;
}

static dx9mt_object_id
dx9mt_ib_object_id_from_iface(IDirect3DIndexBuffer9 *iface) {
  if (!iface) {
    return 0;
  }
  return dx9mt_ib_from_iface(iface)->object_id;
}

static dx9mt_object_id
dx9mt_vdecl_object_id_from_iface(IDirect3DVertexDeclaration9 *iface) {
  if (!iface) {
    return 0;
  }
  return dx9mt_vdecl_from_iface(iface)->object_id;
}

static dx9mt_object_id
dx9mt_vshader_object_id_from_iface(IDirect3DVertexShader9 *iface) {
  if (!iface) {
    return 0;
  }
  return dx9mt_vshader_from_iface(iface)->object_id;
}

static dx9mt_object_id
dx9mt_pshader_object_id_from_iface(IDirect3DPixelShader9 *iface) {
  if (!iface) {
    return 0;
  }
  return dx9mt_pshader_from_iface(iface)->object_id;
}

static dx9mt_object_id
dx9mt_texture_object_id_from_base_iface(IDirect3DBaseTexture9 *iface) {
  D3DRESOURCETYPE type;

  if (!iface) {
    return 0;
  }

  type = IDirect3DBaseTexture9_GetType(iface);
  if (type == D3DRTYPE_TEXTURE) {
    return dx9mt_texture_from_iface((IDirect3DTexture9 *)iface)->object_id;
  }
  if (type == D3DRTYPE_CUBETEXTURE) {
    return dx9mt_cube_texture_from_iface((IDirect3DCubeTexture9 *)iface)->object_id;
  }

  return 0;
}

static dx9mt_object_id
dx9mt_surface_container_texture_id(const dx9mt_surface *surface) {
  IUnknown *container;
  IDirect3DBaseTexture9 *base_texture = NULL;
  dx9mt_object_id texture_id = 0;

  if (!surface) {
    return 0;
  }

  container = surface->container;
  if (!container) {
    return 0;
  }

  if (FAILED(container->lpVtbl->QueryInterface(
          container, &IID_IDirect3DBaseTexture9, (void **)&base_texture)) ||
      !base_texture) {
    return 0;
  }

  texture_id = dx9mt_texture_object_id_from_base_iface(base_texture);
  IDirect3DBaseTexture9_Release(base_texture);
  return texture_id;
}

static uint32_t dx9mt_hash_u32(uint32_t hash, uint32_t value) {
  hash ^= value;
  hash *= 16777619u;
  return hash;
}

static uint32_t dx9mt_align_up_u32(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

static void dx9mt_frontend_upload_begin_frame(uint32_t frame_id) {
  dx9mt_frontend_upload_ensure();
  if (!g_frontend_upload_state) {
    return;
  }
  if (g_frontend_upload_state->frame_id == frame_id) {
    return;
  }

  g_frontend_upload_state->frame_id = frame_id;
  g_frontend_upload_state->slot_index =
      (uint16_t)(frame_id % DX9MT_UPLOAD_ARENA_SLOTS);
  g_frontend_upload_state->next_offset = 0;
}

static dx9mt_upload_ref dx9mt_frontend_upload_copy(uint32_t frame_id,
                                                   const void *data,
                                                   uint32_t size) {
  dx9mt_upload_ref ref;
  uint32_t aligned_size;
  unsigned char *slot_base;

  memset(&ref, 0, sizeof(ref));
  if (!data || size == 0 || size > DX9MT_UPLOAD_BYTES_PER_SLOT) {
    return ref;
  }

  aligned_size = dx9mt_align_up_u32(size, 16u);
  if (aligned_size > DX9MT_UPLOAD_BYTES_PER_SLOT) {
    return ref;
  }

  dx9mt_frontend_upload_begin_frame(frame_id);

  /*
   * Slot overflow: if this allocation doesn't fit in the remaining space,
   * return a zero-ref instead of silently wrapping to offset 0. Wrapping
   * would overwrite earlier constant uploads from the same frame, causing
   * the backend to read corrupted shader constant data with no error.
   *
   * At ~19 draws/frame * 8KB constants = ~152KB per frame this won't trigger
   * for FNV (slot is 1MB), but a heavier game could hit it. The backend
   * validates upload refs and will reject draws with zero-size refs, so this
   * surfaces cleanly in logs rather than producing silent visual corruption.
   */
  if (g_frontend_upload_state->next_offset >
      DX9MT_UPLOAD_BYTES_PER_SLOT - aligned_size) {
    static LONG overflow_counter = 0;
    if (dx9mt_should_log_method_sample(&overflow_counter, 4, 256)) {
      dx9mt_logf("upload",
                 "slot overflow: frame=%u slot=%u offset=%u need=%u capacity=%u",
                 frame_id, (unsigned)g_frontend_upload_state->slot_index,
                 g_frontend_upload_state->next_offset, aligned_size,
                 DX9MT_UPLOAD_BYTES_PER_SLOT);
    }
    return ref;
  }

  slot_base = g_frontend_upload_state->slots[g_frontend_upload_state->slot_index];
  memcpy(slot_base + g_frontend_upload_state->next_offset, data, size);
  ref.arena_index = g_frontend_upload_state->slot_index;
  ref.offset = g_frontend_upload_state->next_offset;
  ref.size = size;
  g_frontend_upload_state->next_offset += aligned_size;
  return ref;
}

static uint32_t dx9mt_hash_texture_stage_state(const dx9mt_device *self) {
  uint32_t hash = 2166136261u;
  uint32_t stage;
  uint32_t type;

  if (!self) {
    return 0;
  }

  for (stage = 0; stage < DX9MT_MAX_TEXTURE_STAGES; ++stage) {
    hash =
        dx9mt_hash_u32(hash, dx9mt_texture_object_id_from_base_iface(self->textures[stage]));
    for (type = 0; type < DX9MT_MAX_TEXTURE_STAGE_STATES; ++type) {
      hash = dx9mt_hash_u32(hash, self->tex_stage_states[stage][type]);
    }
  }

  return hash;
}

static uint32_t dx9mt_hash_sampler_state(const dx9mt_device *self) {
  uint32_t hash = 2166136261u;
  uint32_t sampler;
  uint32_t type;

  if (!self) {
    return 0;
  }

  for (sampler = 0; sampler < DX9MT_MAX_SAMPLERS; ++sampler) {
    for (type = 0; type < DX9MT_MAX_SAMPLER_STATES; ++type) {
      hash = dx9mt_hash_u32(hash, self->sampler_states[sampler][type]);
    }
  }

  return hash;
}

static uint32_t dx9mt_hash_stream_bindings(const dx9mt_device *self) {
  uint32_t hash = 2166136261u;
  uint32_t stream_index;

  if (!self) {
    return 0;
  }

  for (stream_index = 0; stream_index < DX9MT_MAX_STREAMS; ++stream_index) {
    hash = dx9mt_hash_u32(
        hash, dx9mt_vb_object_id_from_iface(self->streams[stream_index]));
    hash = dx9mt_hash_u32(hash, self->stream_offsets[stream_index]);
    hash = dx9mt_hash_u32(hash, self->stream_strides[stream_index]);
    hash = dx9mt_hash_u32(hash, self->stream_freq[stream_index]);
  }

  return hash;
}

static uint32_t dx9mt_hash_float_bits(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static uint32_t dx9mt_hash_viewport(const D3DVIEWPORT9 *viewport) {
  uint32_t hash = 2166136261u;

  if (!viewport) {
    return 0;
  }

  hash = dx9mt_hash_u32(hash, viewport->X);
  hash = dx9mt_hash_u32(hash, viewport->Y);
  hash = dx9mt_hash_u32(hash, viewport->Width);
  hash = dx9mt_hash_u32(hash, viewport->Height);
  hash = dx9mt_hash_u32(hash, dx9mt_hash_float_bits(viewport->MinZ));
  hash = dx9mt_hash_u32(hash, dx9mt_hash_float_bits(viewport->MaxZ));
  return hash;
}

static uint32_t dx9mt_hash_rect(const RECT *rect) {
  uint32_t hash = 2166136261u;

  if (!rect) {
    return 0;
  }

  hash = dx9mt_hash_u32(hash, (uint32_t)rect->left);
  hash = dx9mt_hash_u32(hash, (uint32_t)rect->top);
  hash = dx9mt_hash_u32(hash, (uint32_t)rect->right);
  hash = dx9mt_hash_u32(hash, (uint32_t)rect->bottom);
  return hash;
}

static uint32_t
dx9mt_hash_draw_state(const dx9mt_packet_draw_indexed *packet) {
  uint32_t hash = 2166136261u;

  if (!packet) {
    return 0;
  }

  hash = dx9mt_hash_u32(hash, packet->render_target_id);
  hash = dx9mt_hash_u32(hash, packet->depth_stencil_id);
  hash = dx9mt_hash_u32(hash, packet->render_target_texture_id);
  hash = dx9mt_hash_u32(hash, packet->render_target_width);
  hash = dx9mt_hash_u32(hash, packet->render_target_height);
  hash = dx9mt_hash_u32(hash, packet->render_target_format);
  hash = dx9mt_hash_u32(hash, packet->vertex_buffer_id);
  hash = dx9mt_hash_u32(hash, packet->index_buffer_id);
  hash = dx9mt_hash_u32(hash, packet->vertex_decl_id);
  hash = dx9mt_hash_u32(hash, packet->vertex_shader_id);
  hash = dx9mt_hash_u32(hash, packet->pixel_shader_id);
  hash = dx9mt_hash_u32(hash, packet->fvf);
  hash = dx9mt_hash_u32(hash, packet->stream0_offset);
  hash = dx9mt_hash_u32(hash, packet->stream0_stride);
  hash = dx9mt_hash_u32(hash, packet->primitive_type);
  hash = dx9mt_hash_u32(hash, packet->viewport_hash);
  hash = dx9mt_hash_u32(hash, packet->scissor_hash);
  hash = dx9mt_hash_u32(hash, packet->texture_stage_hash);
  hash = dx9mt_hash_u32(hash, packet->sampler_state_hash);
  hash = dx9mt_hash_u32(hash, packet->stream_binding_hash);
  for (uint32_t s = 0; s < DX9MT_MAX_PS_SAMPLERS; ++s) {
    hash = dx9mt_hash_u32(hash, packet->tex_id[s]);
    hash = dx9mt_hash_u32(hash, packet->tex_generation[s]);
    hash = dx9mt_hash_u32(hash, packet->sampler_min_filter[s]);
    hash = dx9mt_hash_u32(hash, packet->sampler_mag_filter[s]);
    hash = dx9mt_hash_u32(hash, packet->sampler_mip_filter[s]);
    hash = dx9mt_hash_u32(hash, packet->sampler_address_u[s]);
    hash = dx9mt_hash_u32(hash, packet->sampler_address_v[s]);
    hash = dx9mt_hash_u32(hash, packet->sampler_address_w[s]);
  }
  hash = dx9mt_hash_u32(hash, packet->tss0_color_op);
  hash = dx9mt_hash_u32(hash, packet->tss0_color_arg1);
  hash = dx9mt_hash_u32(hash, packet->tss0_color_arg2);
  hash = dx9mt_hash_u32(hash, packet->tss0_alpha_op);
  hash = dx9mt_hash_u32(hash, packet->tss0_alpha_arg1);
  hash = dx9mt_hash_u32(hash, packet->tss0_alpha_arg2);
  hash = dx9mt_hash_u32(hash, packet->rs_texture_factor);
  hash = dx9mt_hash_u32(hash, packet->rs_alpha_blend_enable);
  hash = dx9mt_hash_u32(hash, packet->rs_src_blend);
  hash = dx9mt_hash_u32(hash, packet->rs_dest_blend);
  hash = dx9mt_hash_u32(hash, packet->rs_alpha_test_enable);
  hash = dx9mt_hash_u32(hash, packet->rs_alpha_ref);
  hash = dx9mt_hash_u32(hash, packet->rs_alpha_func);
  hash = dx9mt_hash_u32(hash, packet->rs_zenable);
  hash = dx9mt_hash_u32(hash, packet->rs_zwriteenable);
  hash = dx9mt_hash_u32(hash, packet->rs_zfunc);
  hash = dx9mt_hash_u32(hash, packet->rs_stencilenable);
  hash = dx9mt_hash_u32(hash, packet->rs_stencilfunc);
  hash = dx9mt_hash_u32(hash, packet->rs_stencilref);
  hash = dx9mt_hash_u32(hash, packet->rs_stencilmask);
  hash = dx9mt_hash_u32(hash, packet->rs_stencilwritemask);
  hash = dx9mt_hash_u32(hash, packet->rs_cull_mode);
  hash = dx9mt_hash_u32(hash, packet->rs_scissortestenable);
  hash = dx9mt_hash_u32(hash, packet->rs_blendop);
  hash = dx9mt_hash_u32(hash, packet->rs_colorwriteenable);
  hash = dx9mt_hash_u32(hash, packet->rs_stencilpass);
  hash = dx9mt_hash_u32(hash, packet->rs_stencilfail);
  hash = dx9mt_hash_u32(hash, packet->rs_stencilzfail);
  hash = dx9mt_hash_u32(hash, packet->rs_fogenable);
  hash = dx9mt_hash_u32(hash, packet->rs_fogcolor);
  return hash;
}

static UINT dx9mt_bytes_per_pixel(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8:
  case D3DFMT_D24S8:
  case D3DFMT_D24X8:
    return 4;
  case D3DFMT_R5G6B5:
  case D3DFMT_A1R5G5B5:
  case D3DFMT_X1R5G5B5:
    return 2;
  case D3DFMT_A8:
    return 1;
  default:
    return 4;
  }
}

static WINBOOL dx9mt_format_is_block_compressed(D3DFORMAT format) {
  return format == D3DFMT_DXT1 || format == D3DFMT_DXT3 ||
         format == D3DFMT_DXT5;
}

static UINT dx9mt_format_block_bytes(D3DFORMAT format) {
  if (format == D3DFMT_DXT1) {
    return 8;
  }
  if (format == D3DFMT_DXT3 || format == D3DFMT_DXT5) {
    return 16;
  }
  return 0;
}

static uint32_t dx9mt_texture_next_generation(uint32_t generation) {
  ++generation;
  if (generation == 0) {
    generation = 1;
  }
  return generation;
}

static uint32_t dx9mt_surface_upload_size_from_desc(const D3DSURFACE_DESC *desc,
                                                    UINT pitch) {
  uint32_t block_rows;

  if (!desc || pitch == 0) {
    return 0;
  }
  if (dx9mt_format_is_block_compressed(desc->Format)) {
    block_rows = (desc->Height + 3u) / 4u;
    if (block_rows == 0) {
      block_rows = 1;
    }
    return (uint32_t)(pitch * block_rows);
  }
  return (uint32_t)(pitch * desc->Height);
}

static uint32_t dx9mt_surface_upload_size(const dx9mt_surface *surface) {
  if (!surface) {
    return 0;
  }
  return dx9mt_surface_upload_size_from_desc(&surface->desc, surface->pitch);
}

static void dx9mt_texture_mark_dirty(dx9mt_texture *texture) {
  if (!texture) {
    return;
  }
  texture->generation = dx9mt_texture_next_generation(texture->generation);
}

static void dx9mt_cube_texture_mark_dirty(dx9mt_cube_texture *texture) {
  if (!texture) {
    return;
  }
  texture->generation = dx9mt_texture_next_generation(texture->generation);
}

static void dx9mt_surface_mark_container_dirty(dx9mt_surface *surface) {
  IDirect3DBaseTexture9 *base = NULL;
  HRESULT hr;

  if (!surface || !surface->container) {
    return;
  }

  hr = surface->container->lpVtbl->QueryInterface(
      surface->container, &IID_IDirect3DBaseTexture9, (void **)&base);
  if (FAILED(hr) || !base) {
    return;
  }

  switch (IDirect3DBaseTexture9_GetType(base)) {
  case D3DRTYPE_TEXTURE:
    dx9mt_texture_mark_dirty(
        dx9mt_texture_from_iface((IDirect3DTexture9 *)base));
    break;
  case D3DRTYPE_CUBETEXTURE:
    dx9mt_cube_texture_mark_dirty(
        dx9mt_cube_texture_from_iface((IDirect3DCubeTexture9 *)base));
    break;
  default:
    break;
  }

  IDirect3DBaseTexture9_Release(base);
}

static int dx9mt_env_flag_enabled(const char *name) {
  const char *value;

  if (!name) {
    return 0;
  }

  value = getenv(name);
  if (!value || !*value || strcmp(value, "0") == 0 ||
      strcmp(value, "false") == 0 || strcmp(value, "FALSE") == 0 ||
      strcmp(value, "off") == 0 || strcmp(value, "OFF") == 0 ||
      strcmp(value, "no") == 0 || strcmp(value, "NO") == 0) {
    return 0;
  }

  return 1;
}

static int dx9mt_frontend_soft_present_enabled(void) {
  static LONG cached = -1;
  LONG current;

  current = InterlockedCompareExchange(&cached, -1, -1);
  if (current >= 0) {
    return (int)current;
  }

  current = dx9mt_env_flag_enabled("DX9MT_FRONTEND_SOFT_PRESENT") ? 1 : 0;
  InterlockedExchange(&cached, current);
  return (int)current;
}

static UINT dx9mt_surface_pitch(const D3DSURFACE_DESC *desc) {
  UINT block_bytes;
  UINT block_columns;

  if (!desc || desc->Width == 0) {
    return 0;
  }
  if (!dx9mt_format_is_block_compressed(desc->Format)) {
    return desc->Width * dx9mt_bytes_per_pixel(desc->Format);
  }

  block_bytes = dx9mt_format_block_bytes(desc->Format);
  block_columns = (desc->Width + 3u) / 4u;
  if (block_columns == 0) {
    block_columns = 1;
  }
  return block_columns * block_bytes;
}

static HRESULT dx9mt_safe_addref(IUnknown *obj) {
  if (!obj) {
    return D3D_OK;
  }
  obj->lpVtbl->AddRef(obj);
  return D3D_OK;
}

static void dx9mt_safe_release(IUnknown *obj) {
  if (obj) {
    obj->lpVtbl->Release(obj);
  }
}

/*
 * Scan D3D9 shader bytecode for the END token (0x0000FFFF).
 *
 * D3D9 vertex shaders start with 0xFFFE0300 (vs_3_0) and pixel shaders
 * with 0xFFFF0300 (ps_3_0). We validate the version token first to avoid
 * scanning up to 4MB of arbitrary memory if the caller passes garbage.
 * The scan limit (64K DWORDs = 256KB) is generous for SM3.0 which supports
 * at most 32768 instruction slots.
 */
#define DX9MT_SHADER_MAX_SCAN_DWORDS (1u << 16)

static UINT dx9mt_shader_dword_count(const DWORD *byte_code) {
  UINT i;
  DWORD version_token;

  if (!byte_code) {
    return 0;
  }

  version_token = byte_code[0];
  if ((version_token & 0xFFFF0000u) != 0xFFFE0000u &&
      (version_token & 0xFFFF0000u) != 0xFFFF0000u) {
    dx9mt_logf("device", "shader bytecode bad version token: 0x%08x",
               (unsigned)version_token);
    return 0;
  }

  for (i = 1; i < DX9MT_SHADER_MAX_SCAN_DWORDS; ++i) {
    if (byte_code[i] == 0x0000FFFFu) {
      return i + 1;
    }
  }

  dx9mt_logf("device",
             "shader bytecode END token not found within %u DWORDs (version=0x%08x)",
             DX9MT_SHADER_MAX_SCAN_DWORDS, (unsigned)version_token);
  return 0;
}

static HRESULT dx9mt_surface_ensure_sysmem(dx9mt_surface *surface) {
  SIZE_T size;

  if (!surface) {
    return D3DERR_INVALIDCALL;
  }

  if (surface->sysmem) {
    return D3D_OK;
  }

  size = (SIZE_T)dx9mt_surface_upload_size(surface);
  if (size == 0) {
    return D3D_OK;
  }

  surface->sysmem = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                               size);
  if (!surface->sysmem) {
    return E_OUTOFMEMORY;
  }

  return D3D_OK;
}

static void dx9mt_resolve_rect(const D3DSURFACE_DESC *desc, const RECT *input,
                               RECT *output) {
  if (!input) {
    output->left = 0;
    output->top = 0;
    output->right = (LONG)desc->Width;
    output->bottom = (LONG)desc->Height;
    return;
  }

  *output = *input;
}

static WINBOOL dx9mt_rect_valid_for_surface(const RECT *rect,
                                            const D3DSURFACE_DESC *desc) {
  if (!rect || !desc) {
    return FALSE;
  }

  if (rect->left < 0 || rect->top < 0 || rect->right <= rect->left ||
      rect->bottom <= rect->top) {
    return FALSE;
  }

  if ((UINT)rect->right > desc->Width || (UINT)rect->bottom > desc->Height) {
    return FALSE;
  }

  return TRUE;
}

static HRESULT dx9mt_surface_copy_rect(dx9mt_surface *dst, const RECT *dst_rect,
                                       dx9mt_surface *src, const RECT *src_rect,
                                       WINBOOL allow_scale) {
  RECT src_r;
  RECT dst_r;
  UINT src_bpp;
  UINT dst_bpp;
  UINT src_w;
  UINT src_h;
  UINT dst_w;
  UINT dst_h;
  HRESULT hr;
  UINT x;
  UINT y;

  if (!dst || !src) {
    return D3DERR_INVALIDCALL;
  }

  src_bpp = dx9mt_bytes_per_pixel(src->desc.Format);
  dst_bpp = dx9mt_bytes_per_pixel(dst->desc.Format);
  if (src_bpp != dst_bpp) {
    return D3DERR_INVALIDCALL;
  }

  dx9mt_resolve_rect(&src->desc, src_rect, &src_r);
  dx9mt_resolve_rect(&dst->desc, dst_rect, &dst_r);
  if (!dx9mt_rect_valid_for_surface(&src_r, &src->desc) ||
      !dx9mt_rect_valid_for_surface(&dst_r, &dst->desc)) {
    return D3DERR_INVALIDCALL;
  }

  src_w = (UINT)(src_r.right - src_r.left);
  src_h = (UINT)(src_r.bottom - src_r.top);
  dst_w = (UINT)(dst_r.right - dst_r.left);
  dst_h = (UINT)(dst_r.bottom - dst_r.top);

  if (!allow_scale && (src_w != dst_w || src_h != dst_h)) {
    return D3DERR_INVALIDCALL;
  }

  hr = dx9mt_surface_ensure_sysmem(src);
  if (FAILED(hr)) {
    return hr;
  }

  hr = dx9mt_surface_ensure_sysmem(dst);
  if (FAILED(hr)) {
    return hr;
  }

  if (src_w == dst_w && src_h == dst_h) {
    UINT row_bytes = src_w * src_bpp;
    for (y = 0; y < src_h; ++y) {
      unsigned char *src_row = src->sysmem + (SIZE_T)(src_r.top + (LONG)y) * src->pitch +
                               (SIZE_T)src_r.left * src_bpp;
      unsigned char *dst_row = dst->sysmem + (SIZE_T)(dst_r.top + (LONG)y) * dst->pitch +
                               (SIZE_T)dst_r.left * dst_bpp;
      memmove(dst_row, src_row, row_bytes);
    }
    dx9mt_surface_mark_container_dirty(dst);
    return D3D_OK;
  }

  for (y = 0; y < dst_h; ++y) {
    UINT src_y = (UINT)src_r.top + (y * src_h) / dst_h;
    unsigned char *dst_row =
        dst->sysmem + (SIZE_T)(dst_r.top + (LONG)y) * dst->pitch + (SIZE_T)dst_r.left * dst_bpp;
    const unsigned char *src_row =
        src->sysmem + (SIZE_T)src_y * src->pitch + (SIZE_T)src_r.left * src_bpp;
    for (x = 0; x < dst_w; ++x) {
      UINT src_x = (x * src_w) / dst_w;
      memmove(dst_row + (SIZE_T)x * dst_bpp, src_row + (SIZE_T)src_x * src_bpp,
              dst_bpp);
    }
  }

  dx9mt_surface_mark_container_dirty(dst);
  return D3D_OK;
}

static UINT dx9mt_resolve_backbuffer_width(const D3DPRESENT_PARAMETERS *params) {
  return params && params->BackBufferWidth ? params->BackBufferWidth : 1280;
}

static UINT dx9mt_resolve_backbuffer_height(const D3DPRESENT_PARAMETERS *params) {
  return params && params->BackBufferHeight ? params->BackBufferHeight : 720;
}

static D3DFORMAT
dx9mt_resolve_backbuffer_format(const D3DPRESENT_PARAMETERS *params) {
  D3DFORMAT format;

  format = params ? params->BackBufferFormat : D3DFMT_UNKNOWN;
  if (format == D3DFMT_UNKNOWN) {
    format = D3DFMT_X8R8G8B8;
  }
  return format;
}

static HRESULT dx9mt_device_publish_present_target(dx9mt_device *self) {
  dx9mt_backend_present_target_desc desc;
  HWND present_window;

  if (!self) {
    return D3DERR_INVALIDCALL;
  }
  if (self->present_target_id == 0) {
    if (self->swapchain && self->swapchain->object_id != 0) {
      self->present_target_id = self->swapchain->object_id;
    } else {
      self->present_target_id = dx9mt_alloc_object_id(DX9MT_OBJECT_KIND_SWAPCHAIN);
    }
  }

  memset(&desc, 0, sizeof(desc));
  present_window =
      self->params.hDeviceWindow ? self->params.hDeviceWindow : self->focus_window;
  desc.target_id = self->present_target_id;
  desc.window_handle = (uint64_t)(uintptr_t)present_window;
  desc.width = dx9mt_resolve_backbuffer_width(&self->params);
  desc.height = dx9mt_resolve_backbuffer_height(&self->params);
  desc.format = (uint32_t)dx9mt_resolve_backbuffer_format(&self->params);
  desc.windowed = self->params.Windowed ? 1u : 0u;

  if (dx9mt_backend_bridge_update_present_target(&desc) != 0) {
    dx9mt_logf(
        "device",
        "failed to publish present target metadata target=%llu hwnd=0x%llx size=%ux%u fmt=%u windowed=%u",
        (unsigned long long)desc.target_id,
        (unsigned long long)desc.window_handle, desc.width, desc.height, desc.format,
        desc.windowed);
    return D3DERR_DRIVERINTERNALERROR;
  }

  return D3D_OK;
}

static HRESULT dx9mt_surface_fill_rect(dx9mt_surface *surface, const RECT *rect,
                                       D3DCOLOR color) {
  RECT fill_rect;
  HRESULT hr;
  UINT bpp;
  UINT width;
  UINT height;
  UINT x;
  UINT y;

  if (!surface) {
    return D3DERR_INVALIDCALL;
  }

  dx9mt_resolve_rect(&surface->desc, rect, &fill_rect);
  if (!dx9mt_rect_valid_for_surface(&fill_rect, &surface->desc)) {
    return D3DERR_INVALIDCALL;
  }

  hr = dx9mt_surface_ensure_sysmem(surface);
  if (FAILED(hr)) {
    return hr;
  }

  bpp = dx9mt_bytes_per_pixel(surface->desc.Format);
  width = (UINT)(fill_rect.right - fill_rect.left);
  height = (UINT)(fill_rect.bottom - fill_rect.top);

  for (y = 0; y < height; ++y) {
    unsigned char *dst_row = surface->sysmem +
                             (SIZE_T)(fill_rect.top + (LONG)y) * surface->pitch +
                             (SIZE_T)fill_rect.left * bpp;
    if (bpp == 4) {
      DWORD *row32 = (DWORD *)dst_row;
      for (x = 0; x < width; ++x) {
        row32[x] = color;
      }
    } else if (bpp == 2) {
      WORD value16 = (WORD)(color & 0xFFFFu);
      WORD *row16 = (WORD *)dst_row;
      for (x = 0; x < width; ++x) {
        row16[x] = value16;
      }
    } else {
      BYTE value8 = (BYTE)(color & 0xFFu);
      memset(dst_row, value8, width);
    }
  }

  dx9mt_surface_mark_container_dirty(surface);
  return D3D_OK;
}

static HWND dx9mt_device_resolve_present_window(const dx9mt_device *self,
                                                HWND dst_window_override) {
  if (!self) {
    return NULL;
  }
  if (dst_window_override) {
    return dst_window_override;
  }
  if (self->params.hDeviceWindow) {
    return self->params.hDeviceWindow;
  }
  return self->focus_window;
}

static void dx9mt_surface_apply_debug_overlay(dx9mt_surface *surface,
                                              uint32_t frame_id) {
  UINT overlay_w;
  UINT overlay_h;
  UINT x;
  UINT y;
  DWORD base_color;

  if (!surface || !surface->sysmem || dx9mt_bytes_per_pixel(surface->desc.Format) != 4) {
    return;
  }

  overlay_w = surface->desc.Width < 96u ? surface->desc.Width : 96u;
  overlay_h = surface->desc.Height < 16u ? surface->desc.Height : 16u;
  if (overlay_w == 0 || overlay_h == 0) {
    return;
  }

  base_color = 0xFF000000u | (((frame_id * 13u) & 0xFFu) << 16) |
               (((frame_id * 29u) & 0xFFu) << 8) | ((frame_id * 47u) & 0xFFu);

  for (y = 0; y < overlay_h; ++y) {
    DWORD *row = (DWORD *)(surface->sysmem + (SIZE_T)y * surface->pitch);
    for (x = 0; x < overlay_w; ++x) {
      DWORD color = base_color;
      if (((x >> 3) + y + frame_id) & 1u) {
        color ^= 0x00FFFFFFu;
      }
      row[x] = color;
    }
  }
}

static HRESULT dx9mt_device_soft_present(dx9mt_device *self,
                                         HWND dst_window_override) {
  dx9mt_surface *backbuffer;
  HWND hwnd;
  HDC hdc;
  BITMAPINFO bmi;
  HRESULT hr;
  int stretch_result;
  static LONG log_counter = 0;

  if (!self || !dx9mt_frontend_soft_present_enabled()) {
    return D3D_OK;
  }
  if (!self->swapchain || !self->swapchain->backbuffer) {
    return D3D_OK;
  }

  backbuffer = dx9mt_surface_from_iface(self->swapchain->backbuffer);
  if (!backbuffer) {
    return D3D_OK;
  }

  hr = dx9mt_surface_ensure_sysmem(backbuffer);
  if (FAILED(hr)) {
    return hr;
  }
  if (dx9mt_bytes_per_pixel(backbuffer->desc.Format) != 4) {
    if (dx9mt_should_log_method_sample(&log_counter, 4, 256)) {
      dx9mt_logf("device", "soft present skipped unsupported format=%u",
                 (unsigned)backbuffer->desc.Format);
    }
    return D3D_OK;
  }

  dx9mt_surface_apply_debug_overlay(backbuffer, self->frame_id);

  hwnd = dx9mt_device_resolve_present_window(self, dst_window_override);
  if (!hwnd || !IsWindow(hwnd)) {
    if (dx9mt_should_log_method_sample(&log_counter, 4, 256)) {
      dx9mt_logf("device", "soft present skipped invalid window hwnd=%p", hwnd);
    }
    return D3D_OK;
  }

  hdc = GetDC(hwnd);
  if (!hdc) {
    if (dx9mt_should_log_method_sample(&log_counter, 4, 256)) {
      dx9mt_logf("device", "soft present GetDC failed hwnd=%p", hwnd);
    }
    return D3D_OK;
  }

  memset(&bmi, 0, sizeof(bmi));
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = (LONG)backbuffer->desc.Width;
  bmi.bmiHeader.biHeight = -(LONG)backbuffer->desc.Height;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  stretch_result = StretchDIBits(
      hdc, 0, 0, (int)backbuffer->desc.Width, (int)backbuffer->desc.Height, 0, 0,
      (int)backbuffer->desc.Width, (int)backbuffer->desc.Height, backbuffer->sysmem,
      &bmi, DIB_RGB_COLORS, SRCCOPY);
  ReleaseDC(hwnd, hdc);

  if (stretch_result <= 0 && dx9mt_should_log_method_sample(&log_counter, 4, 256)) {
    dx9mt_logf("device", "soft present StretchDIBits failed result=%d frame=%u",
               stretch_result, self->frame_id);
  }

  return D3D_OK;
}

/* -------------------------------------------------------------------------- */
/* IDirect3DSurface9                                                          */
/* -------------------------------------------------------------------------- */

static HRESULT WINAPI dx9mt_surface_QueryInterface(IDirect3DSurface9 *iface,
                                                    REFIID riid,
                                                    void **ppv_object);
static ULONG WINAPI dx9mt_surface_AddRef(IDirect3DSurface9 *iface);
static ULONG WINAPI dx9mt_surface_Release(IDirect3DSurface9 *iface);
static HRESULT WINAPI dx9mt_surface_GetDevice(IDirect3DSurface9 *iface,
                                               IDirect3DDevice9 **pp_device);
static HRESULT WINAPI dx9mt_surface_SetPrivateData(IDirect3DSurface9 *iface,
                                                    REFGUID guid,
                                                    const void *data,
                                                    DWORD data_size,
                                                    DWORD flags);
static HRESULT WINAPI dx9mt_surface_GetPrivateData(IDirect3DSurface9 *iface,
                                                    REFGUID guid,
                                                    void *data,
                                                    DWORD *data_size);
static HRESULT WINAPI dx9mt_surface_FreePrivateData(IDirect3DSurface9 *iface,
                                                     REFGUID guid);
static DWORD WINAPI dx9mt_surface_SetPriority(IDirect3DSurface9 *iface,
                                              DWORD priority_new);
static DWORD WINAPI dx9mt_surface_GetPriority(IDirect3DSurface9 *iface);
static void WINAPI dx9mt_surface_PreLoad(IDirect3DSurface9 *iface);
static D3DRESOURCETYPE WINAPI dx9mt_surface_GetType(IDirect3DSurface9 *iface);
static HRESULT WINAPI dx9mt_surface_GetContainer(IDirect3DSurface9 *iface,
                                                  REFIID riid,
                                                  void **pp_container);
static HRESULT WINAPI dx9mt_surface_GetDesc(IDirect3DSurface9 *iface,
                                             D3DSURFACE_DESC *desc);
static HRESULT WINAPI dx9mt_surface_LockRect(IDirect3DSurface9 *iface,
                                              D3DLOCKED_RECT *locked_rect,
                                              const RECT *rect,
                                              DWORD flags);
static HRESULT WINAPI dx9mt_surface_UnlockRect(IDirect3DSurface9 *iface);
static HRESULT WINAPI dx9mt_surface_GetDC(IDirect3DSurface9 *iface, HDC *phdc);
static HRESULT WINAPI dx9mt_surface_ReleaseDC(IDirect3DSurface9 *iface, HDC hdc);

static IDirect3DSurface9Vtbl g_dx9mt_surface_vtbl = {
    dx9mt_surface_QueryInterface,
    dx9mt_surface_AddRef,
    dx9mt_surface_Release,
    dx9mt_surface_GetDevice,
    dx9mt_surface_SetPrivateData,
    dx9mt_surface_GetPrivateData,
    dx9mt_surface_FreePrivateData,
    dx9mt_surface_SetPriority,
    dx9mt_surface_GetPriority,
    dx9mt_surface_PreLoad,
    dx9mt_surface_GetType,
    dx9mt_surface_GetContainer,
    dx9mt_surface_GetDesc,
    dx9mt_surface_LockRect,
    dx9mt_surface_UnlockRect,
    dx9mt_surface_GetDC,
    dx9mt_surface_ReleaseDC,
};

static HRESULT dx9mt_surface_create(dx9mt_device *device, UINT width, UINT height,
                                    D3DFORMAT format, D3DPOOL pool, DWORD usage,
                                    D3DMULTISAMPLE_TYPE multisample,
                                    DWORD multisample_quality, WINBOOL lockable,
                                    IUnknown *container,
                                    IDirect3DSurface9 **out_surface) {
  dx9mt_surface *surface;

  if (!out_surface) {
    return D3DERR_INVALIDCALL;
  }

  *out_surface = NULL;

  surface = (dx9mt_surface *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                       sizeof(dx9mt_surface));
  if (!surface) {
    return E_OUTOFMEMORY;
  }

  surface->iface.lpVtbl = &g_dx9mt_surface_vtbl;
  surface->refcount = 1;
  surface->object_id = dx9mt_alloc_object_id(DX9MT_OBJECT_KIND_SURFACE);
  surface->device = device;
  surface->container = container;
  surface->lockable = lockable;
  surface->desc.Format = format;
  surface->desc.Type = D3DRTYPE_SURFACE;
  surface->desc.Usage = usage;
  surface->desc.Pool = pool;
  surface->desc.MultiSampleType = multisample;
  surface->desc.MultiSampleQuality = multisample_quality;
  surface->desc.Width = width;
  surface->desc.Height = height;

  surface->pitch = dx9mt_surface_pitch(&surface->desc);

  *out_surface = &surface->iface;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_surface_QueryInterface(IDirect3DSurface9 *iface,
                                                    REFIID riid,
                                                    void **ppv_object) {
  if (!ppv_object) {
    return E_POINTER;
  }

  if (IsEqualGUID(riid, &IID_IUnknown) ||
      IsEqualGUID(riid, &IID_IDirect3DResource9) ||
      IsEqualGUID(riid, &IID_IDirect3DSurface9)) {
    *ppv_object = iface;
    dx9mt_surface_AddRef(iface);
    return S_OK;
  }

  *ppv_object = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI dx9mt_surface_AddRef(IDirect3DSurface9 *iface) {
  dx9mt_surface *self = dx9mt_surface_from_iface(iface);
  return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG WINAPI dx9mt_surface_Release(IDirect3DSurface9 *iface) {
  dx9mt_surface *self = dx9mt_surface_from_iface(iface);
  LONG refcount = InterlockedDecrement(&self->refcount);

  if (refcount == 0) {
    HeapFree(GetProcessHeap(), 0, self->sysmem);
    HeapFree(GetProcessHeap(), 0, self);
  }

  return (ULONG)refcount;
}

static HRESULT WINAPI dx9mt_surface_GetDevice(IDirect3DSurface9 *iface,
                                               IDirect3DDevice9 **pp_device) {
  dx9mt_surface *self = dx9mt_surface_from_iface(iface);
  if (!pp_device) {
    return D3DERR_INVALIDCALL;
  }

  *pp_device = self->device ? &self->device->iface : NULL;
  if (*pp_device) {
    IDirect3DDevice9_AddRef(*pp_device);
  }
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_surface_SetPrivateData(IDirect3DSurface9 *iface,
                                                    REFGUID guid,
                                                    const void *data,
                                                    DWORD data_size,
                                                    DWORD flags) {
  (void)iface;
  (void)guid;
  (void)data;
  (void)data_size;
  (void)flags;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_surface_GetPrivateData(IDirect3DSurface9 *iface,
                                                    REFGUID guid,
                                                    void *data,
                                                    DWORD *data_size) {
  (void)iface;
  (void)guid;
  (void)data;
  (void)data_size;
  return D3DERR_NOTFOUND;
}

static HRESULT WINAPI dx9mt_surface_FreePrivateData(IDirect3DSurface9 *iface,
                                                     REFGUID guid) {
  (void)iface;
  (void)guid;
  return D3D_OK;
}

static DWORD WINAPI dx9mt_surface_SetPriority(IDirect3DSurface9 *iface,
                                              DWORD priority_new) {
  (void)iface;
  (void)priority_new;
  return 0;
}

static DWORD WINAPI dx9mt_surface_GetPriority(IDirect3DSurface9 *iface) {
  (void)iface;
  return 0;
}

static void WINAPI dx9mt_surface_PreLoad(IDirect3DSurface9 *iface) {
  (void)iface;
}

static D3DRESOURCETYPE WINAPI dx9mt_surface_GetType(IDirect3DSurface9 *iface) {
  (void)iface;
  return D3DRTYPE_SURFACE;
}

static HRESULT WINAPI dx9mt_surface_GetContainer(IDirect3DSurface9 *iface,
                                                  REFIID riid,
                                                  void **pp_container) {
  dx9mt_surface *self = dx9mt_surface_from_iface(iface);
  if (!pp_container) {
    return D3DERR_INVALIDCALL;
  }

  if (!self->container) {
    *pp_container = NULL;
    return E_NOINTERFACE;
  }

  return self->container->lpVtbl->QueryInterface(self->container, riid,
                                                 pp_container);
}

static HRESULT WINAPI dx9mt_surface_GetDesc(IDirect3DSurface9 *iface,
                                             D3DSURFACE_DESC *desc) {
  dx9mt_surface *self = dx9mt_surface_from_iface(iface);
  if (!desc) {
    return D3DERR_INVALIDCALL;
  }

  *desc = self->desc;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_surface_LockRect(IDirect3DSurface9 *iface,
                                              D3DLOCKED_RECT *locked_rect,
                                              const RECT *rect,
                                              DWORD flags) {
  dx9mt_surface *self = dx9mt_surface_from_iface(iface);
  uint32_t size;

  (void)rect;
  (void)flags;

  if (!locked_rect) {
    return D3DERR_INVALIDCALL;
  }

  if (!self->lockable) {
    return D3DERR_INVALIDCALL;
  }

  if (!self->sysmem) {
    size = dx9mt_surface_upload_size(self);
    self->sysmem = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                              size);
    if (!self->sysmem) {
      return E_OUTOFMEMORY;
    }
  }

  locked_rect->Pitch = (INT)self->pitch;
  locked_rect->pBits = self->sysmem;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_surface_UnlockRect(IDirect3DSurface9 *iface) {
  dx9mt_surface *self = dx9mt_surface_from_iface(iface);
  dx9mt_surface_mark_container_dirty(self);
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_surface_GetDC(IDirect3DSurface9 *iface, HDC *phdc) {
  (void)iface;
  if (phdc) {
    *phdc = NULL;
  }
  return D3DERR_INVALIDCALL;
}

static HRESULT WINAPI dx9mt_surface_ReleaseDC(IDirect3DSurface9 *iface, HDC hdc) {
  (void)iface;
  (void)hdc;
  return D3DERR_INVALIDCALL;
}

/* -------------------------------------------------------------------------- */
/* IDirect3DSwapChain9                                                        */
/* -------------------------------------------------------------------------- */

static HRESULT WINAPI dx9mt_swapchain_QueryInterface(IDirect3DSwapChain9 *iface,
                                                      REFIID riid,
                                                      void **ppv_object);
static ULONG WINAPI dx9mt_swapchain_AddRef(IDirect3DSwapChain9 *iface);
static ULONG WINAPI dx9mt_swapchain_Release(IDirect3DSwapChain9 *iface);
static HRESULT WINAPI dx9mt_swapchain_Present(IDirect3DSwapChain9 *iface,
                                               const RECT *src_rect,
                                               const RECT *dst_rect,
                                               HWND dst_window_override,
                                               const RGNDATA *dirty_region,
                                               DWORD flags);
static HRESULT WINAPI dx9mt_swapchain_GetFrontBufferData(
    IDirect3DSwapChain9 *iface, IDirect3DSurface9 *dest_surface);
static HRESULT WINAPI dx9mt_swapchain_GetBackBuffer(
    IDirect3DSwapChain9 *iface, UINT backbuffer_idx,
    D3DBACKBUFFER_TYPE backbuffer_type, IDirect3DSurface9 **backbuffer);
static HRESULT WINAPI dx9mt_swapchain_GetRasterStatus(
    IDirect3DSwapChain9 *iface, D3DRASTER_STATUS *raster_status);
static HRESULT WINAPI dx9mt_swapchain_GetDisplayMode(IDirect3DSwapChain9 *iface,
                                                      D3DDISPLAYMODE *mode);
static HRESULT WINAPI dx9mt_swapchain_GetDevice(IDirect3DSwapChain9 *iface,
                                                 IDirect3DDevice9 **device);
static HRESULT WINAPI dx9mt_swapchain_GetPresentParameters(
    IDirect3DSwapChain9 *iface, D3DPRESENT_PARAMETERS *parameters);

static IDirect3DSwapChain9Vtbl g_dx9mt_swapchain_vtbl = {
    dx9mt_swapchain_QueryInterface,
    dx9mt_swapchain_AddRef,
    dx9mt_swapchain_Release,
    dx9mt_swapchain_Present,
    dx9mt_swapchain_GetFrontBufferData,
    dx9mt_swapchain_GetBackBuffer,
    dx9mt_swapchain_GetRasterStatus,
    dx9mt_swapchain_GetDisplayMode,
    dx9mt_swapchain_GetDevice,
    dx9mt_swapchain_GetPresentParameters,
};

static HRESULT dx9mt_swapchain_create(dx9mt_device *device,
                                      const D3DPRESENT_PARAMETERS *params,
                                      dx9mt_swapchain **out_swapchain) {
  dx9mt_swapchain *swapchain;
  UINT width;
  UINT height;
  D3DFORMAT format;
  HRESULT hr;

  if (!out_swapchain) {
    return D3DERR_INVALIDCALL;
  }
  *out_swapchain = NULL;

  swapchain = (dx9mt_swapchain *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                            sizeof(dx9mt_swapchain));
  if (!swapchain) {
    return E_OUTOFMEMORY;
  }

  swapchain->iface.lpVtbl = &g_dx9mt_swapchain_vtbl;
  swapchain->refcount = 1;
  swapchain->object_id = dx9mt_alloc_object_id(DX9MT_OBJECT_KIND_SWAPCHAIN);
  swapchain->device = device;
  swapchain->params = *params;

  width = params->BackBufferWidth ? params->BackBufferWidth : 1280;
  height = params->BackBufferHeight ? params->BackBufferHeight : 720;
  format = params->BackBufferFormat;
  if (format == D3DFMT_UNKNOWN) {
    format = D3DFMT_X8R8G8B8;
  }

  hr = dx9mt_surface_create(device, width, height, format, D3DPOOL_DEFAULT,
                            D3DUSAGE_RENDERTARGET, D3DMULTISAMPLE_NONE, 0,
                            FALSE, (IUnknown *)&swapchain->iface,
                            &swapchain->backbuffer);
  if (FAILED(hr)) {
    HeapFree(GetProcessHeap(), 0, swapchain);
    return hr;
  }

  *out_swapchain = swapchain;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_swapchain_QueryInterface(IDirect3DSwapChain9 *iface,
                                                      REFIID riid,
                                                      void **ppv_object) {
  if (!ppv_object) {
    return E_POINTER;
  }

  if (IsEqualGUID(riid, &IID_IUnknown) ||
      IsEqualGUID(riid, &IID_IDirect3DSwapChain9)) {
    *ppv_object = iface;
    dx9mt_swapchain_AddRef(iface);
    return S_OK;
  }

  *ppv_object = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI dx9mt_swapchain_AddRef(IDirect3DSwapChain9 *iface) {
  dx9mt_swapchain *self = dx9mt_swapchain_from_iface(iface);
  return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG WINAPI dx9mt_swapchain_Release(IDirect3DSwapChain9 *iface) {
  dx9mt_swapchain *self = dx9mt_swapchain_from_iface(iface);
  LONG refcount = InterlockedDecrement(&self->refcount);

  if (refcount == 0) {
    if (self->backbuffer) {
      dx9mt_surface *surface = dx9mt_surface_from_iface(self->backbuffer);
      surface->container = NULL;
      IDirect3DSurface9_Release(self->backbuffer);
    }
    HeapFree(GetProcessHeap(), 0, self);
  }

  return (ULONG)refcount;
}

static HRESULT WINAPI dx9mt_swapchain_Present(IDirect3DSwapChain9 *iface,
                                               const RECT *src_rect,
                                               const RECT *dst_rect,
                                               HWND dst_window_override,
                                               const RGNDATA *dirty_region,
                                               DWORD flags) {
  dx9mt_swapchain *self = dx9mt_swapchain_from_iface(iface);
  (void)flags;

  ++self->present_count;
  return IDirect3DDevice9_Present(&self->device->iface, src_rect, dst_rect,
                                  dst_window_override, dirty_region);
}

static HRESULT WINAPI dx9mt_swapchain_GetFrontBufferData(
    IDirect3DSwapChain9 *iface, IDirect3DSurface9 *dest_surface) {
  dx9mt_swapchain *self = dx9mt_swapchain_from_iface(iface);
  if (!dest_surface || !self->backbuffer) {
    return D3DERR_INVALIDCALL;
  }
  return dx9mt_surface_copy_rect(dx9mt_surface_from_iface(dest_surface), NULL,
                                 dx9mt_surface_from_iface(self->backbuffer),
                                 NULL, FALSE);
}

static HRESULT WINAPI dx9mt_swapchain_GetBackBuffer(
    IDirect3DSwapChain9 *iface, UINT backbuffer_idx,
    D3DBACKBUFFER_TYPE backbuffer_type, IDirect3DSurface9 **backbuffer) {
  dx9mt_swapchain *self = dx9mt_swapchain_from_iface(iface);
  (void)backbuffer_type;

  if (!backbuffer || backbuffer_idx != 0 || !self->backbuffer) {
    return D3DERR_INVALIDCALL;
  }

  *backbuffer = self->backbuffer;
  IDirect3DSurface9_AddRef(*backbuffer);
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_swapchain_GetRasterStatus(
    IDirect3DSwapChain9 *iface, D3DRASTER_STATUS *raster_status) {
  (void)iface;
  if (!raster_status) {
    return D3DERR_INVALIDCALL;
  }

  raster_status->InVBlank = FALSE;
  raster_status->ScanLine = 0;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_swapchain_GetDisplayMode(IDirect3DSwapChain9 *iface,
                                                      D3DDISPLAYMODE *mode) {
  dx9mt_swapchain *self = dx9mt_swapchain_from_iface(iface);
  if (!mode) {
    return D3DERR_INVALIDCALL;
  }

  mode->Width = self->params.BackBufferWidth ? self->params.BackBufferWidth : 1280;
  mode->Height = self->params.BackBufferHeight ? self->params.BackBufferHeight : 720;
  mode->RefreshRate = self->params.FullScreen_RefreshRateInHz;
  if (!mode->RefreshRate) {
    mode->RefreshRate = 60;
  }
  mode->Format = self->params.BackBufferFormat;
  if (mode->Format == D3DFMT_UNKNOWN) {
    mode->Format = D3DFMT_X8R8G8B8;
  }
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_swapchain_GetDevice(IDirect3DSwapChain9 *iface,
                                                 IDirect3DDevice9 **device) {
  dx9mt_swapchain *self = dx9mt_swapchain_from_iface(iface);
  if (!device) {
    return D3DERR_INVALIDCALL;
  }

  *device = &self->device->iface;
  IDirect3DDevice9_AddRef(*device);
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_swapchain_GetPresentParameters(
    IDirect3DSwapChain9 *iface, D3DPRESENT_PARAMETERS *parameters) {
  dx9mt_swapchain *self = dx9mt_swapchain_from_iface(iface);
  if (!parameters) {
    return D3DERR_INVALIDCALL;
  }

  *parameters = self->params;
  return D3D_OK;
}

/* -------------------------------------------------------------------------- */
/* IDirect3DTexture9                                                          */
/* -------------------------------------------------------------------------- */

static HRESULT WINAPI dx9mt_texture_QueryInterface(IDirect3DTexture9 *iface,
                                                    REFIID riid,
                                                    void **ppv_object);
static ULONG WINAPI dx9mt_texture_AddRef(IDirect3DTexture9 *iface);
static ULONG WINAPI dx9mt_texture_Release(IDirect3DTexture9 *iface);
static HRESULT WINAPI dx9mt_texture_GetDevice(IDirect3DTexture9 *iface,
                                               IDirect3DDevice9 **pp_device);
static HRESULT WINAPI dx9mt_texture_SetPrivateData(IDirect3DTexture9 *iface,
                                                    REFGUID guid,
                                                    const void *data,
                                                    DWORD data_size,
                                                    DWORD flags);
static HRESULT WINAPI dx9mt_texture_GetPrivateData(IDirect3DTexture9 *iface,
                                                    REFGUID guid,
                                                    void *data,
                                                    DWORD *data_size);
static HRESULT WINAPI dx9mt_texture_FreePrivateData(IDirect3DTexture9 *iface,
                                                     REFGUID guid);
static DWORD WINAPI dx9mt_texture_SetPriority(IDirect3DTexture9 *iface,
                                              DWORD priority_new);
static DWORD WINAPI dx9mt_texture_GetPriority(IDirect3DTexture9 *iface);
static void WINAPI dx9mt_texture_PreLoad(IDirect3DTexture9 *iface);
static D3DRESOURCETYPE WINAPI dx9mt_texture_GetType(IDirect3DTexture9 *iface);
static DWORD WINAPI dx9mt_texture_SetLOD(IDirect3DTexture9 *iface,
                                         DWORD lod_new);
static DWORD WINAPI dx9mt_texture_GetLOD(IDirect3DTexture9 *iface);
static DWORD WINAPI dx9mt_texture_GetLevelCount(IDirect3DTexture9 *iface);
static HRESULT WINAPI dx9mt_texture_SetAutoGenFilterType(
    IDirect3DTexture9 *iface, D3DTEXTUREFILTERTYPE filter_type);
static D3DTEXTUREFILTERTYPE WINAPI dx9mt_texture_GetAutoGenFilterType(
    IDirect3DTexture9 *iface);
static void WINAPI dx9mt_texture_GenerateMipSubLevels(IDirect3DTexture9 *iface);
static HRESULT WINAPI dx9mt_texture_GetLevelDesc(IDirect3DTexture9 *iface,
                                                  UINT level,
                                                  D3DSURFACE_DESC *desc);
static HRESULT WINAPI dx9mt_texture_GetSurfaceLevel(IDirect3DTexture9 *iface,
                                                     UINT level,
                                                     IDirect3DSurface9 **surface);
static HRESULT WINAPI dx9mt_texture_LockRect(IDirect3DTexture9 *iface,
                                              UINT level,
                                              D3DLOCKED_RECT *locked_rect,
                                              const RECT *rect, DWORD flags);
static HRESULT WINAPI dx9mt_texture_UnlockRect(IDirect3DTexture9 *iface,
                                                UINT level);
static HRESULT WINAPI dx9mt_texture_AddDirtyRect(IDirect3DTexture9 *iface,
                                                  const RECT *dirty_rect);

static IDirect3DTexture9Vtbl g_dx9mt_texture_vtbl = {
    dx9mt_texture_QueryInterface,
    dx9mt_texture_AddRef,
    dx9mt_texture_Release,
    dx9mt_texture_GetDevice,
    dx9mt_texture_SetPrivateData,
    dx9mt_texture_GetPrivateData,
    dx9mt_texture_FreePrivateData,
    dx9mt_texture_SetPriority,
    dx9mt_texture_GetPriority,
    dx9mt_texture_PreLoad,
    dx9mt_texture_GetType,
    dx9mt_texture_SetLOD,
    dx9mt_texture_GetLOD,
    dx9mt_texture_GetLevelCount,
    dx9mt_texture_SetAutoGenFilterType,
    dx9mt_texture_GetAutoGenFilterType,
    dx9mt_texture_GenerateMipSubLevels,
    dx9mt_texture_GetLevelDesc,
    dx9mt_texture_GetSurfaceLevel,
    dx9mt_texture_LockRect,
    dx9mt_texture_UnlockRect,
    dx9mt_texture_AddDirtyRect,
};

static HRESULT dx9mt_texture_create(dx9mt_device *device, UINT width, UINT height,
                                    UINT levels, DWORD usage, D3DFORMAT format,
                                    D3DPOOL pool, IDirect3DTexture9 **out_texture) {
  dx9mt_texture *texture;
  UINT i;
  UINT level_w;
  UINT level_h;
  WINBOOL lockable;

  if (!out_texture || width == 0 || height == 0) {
    return D3DERR_INVALIDCALL;
  }
  *out_texture = NULL;

  if (levels == 0) {
    levels = 1;
  }

  texture = (dx9mt_texture *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                       sizeof(dx9mt_texture));
  if (!texture) {
    return E_OUTOFMEMORY;
  }

  texture->surfaces = (IDirect3DSurface9 **)HeapAlloc(
      GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)levels * sizeof(IDirect3DSurface9 *));
  if (!texture->surfaces) {
    HeapFree(GetProcessHeap(), 0, texture);
    return E_OUTOFMEMORY;
  }

  texture->iface.lpVtbl = &g_dx9mt_texture_vtbl;
  texture->refcount = 1;
  texture->object_id = dx9mt_alloc_object_id(DX9MT_OBJECT_KIND_TEXTURE);
  texture->device = device;
  texture->usage = usage;
  texture->format = format;
  texture->pool = pool;
  texture->width = width;
  texture->height = height;
  texture->levels = levels;
  texture->autogen_filter = D3DTEXF_LINEAR;
  texture->generation = 1;
  texture->last_upload_generation = 0;
  texture->last_upload_frame_id = 0;

  lockable = ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) == 0);
  level_w = width;
  level_h = height;

  for (i = 0; i < levels; ++i) {
    HRESULT hr = dx9mt_surface_create(device, level_w, level_h, format, pool, usage,
                                      D3DMULTISAMPLE_NONE, 0, lockable,
                                      (IUnknown *)&texture->iface,
                                      &texture->surfaces[i]);
    if (FAILED(hr)) {
      while (i > 0) {
        --i;
        IDirect3DSurface9_Release(texture->surfaces[i]);
      }
      HeapFree(GetProcessHeap(), 0, texture->surfaces);
      HeapFree(GetProcessHeap(), 0, texture);
      return hr;
    }

    if (level_w > 1) {
      level_w /= 2;
    }
    if (level_h > 1) {
      level_h /= 2;
    }
  }

  *out_texture = &texture->iface;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_texture_QueryInterface(IDirect3DTexture9 *iface,
                                                    REFIID riid,
                                                    void **ppv_object) {
  if (!ppv_object) {
    return E_POINTER;
  }

  if (IsEqualGUID(riid, &IID_IUnknown) ||
      IsEqualGUID(riid, &IID_IDirect3DResource9) ||
      IsEqualGUID(riid, &IID_IDirect3DBaseTexture9) ||
      IsEqualGUID(riid, &IID_IDirect3DTexture9)) {
    *ppv_object = iface;
    dx9mt_texture_AddRef(iface);
    return S_OK;
  }

  *ppv_object = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI dx9mt_texture_AddRef(IDirect3DTexture9 *iface) {
  dx9mt_texture *self = dx9mt_texture_from_iface(iface);
  return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG WINAPI dx9mt_texture_Release(IDirect3DTexture9 *iface) {
  dx9mt_texture *self = dx9mt_texture_from_iface(iface);
  LONG refcount = InterlockedDecrement(&self->refcount);
  UINT i;

  if (refcount == 0) {
    for (i = 0; i < self->levels; ++i) {
      if (self->surfaces[i]) {
        dx9mt_surface *surface = dx9mt_surface_from_iface(self->surfaces[i]);
        surface->container = NULL;
        IDirect3DSurface9_Release(self->surfaces[i]);
      }
    }
    HeapFree(GetProcessHeap(), 0, self->surfaces);
    HeapFree(GetProcessHeap(), 0, self);
  }

  return (ULONG)refcount;
}

static HRESULT WINAPI dx9mt_texture_GetDevice(IDirect3DTexture9 *iface,
                                               IDirect3DDevice9 **pp_device) {
  dx9mt_texture *self = dx9mt_texture_from_iface(iface);
  if (!pp_device) {
    return D3DERR_INVALIDCALL;
  }

  *pp_device = self->device ? &self->device->iface : NULL;
  if (*pp_device) {
    IDirect3DDevice9_AddRef(*pp_device);
  }
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_texture_SetPrivateData(IDirect3DTexture9 *iface,
                                                    REFGUID guid,
                                                    const void *data,
                                                    DWORD data_size,
                                                    DWORD flags) {
  (void)iface;
  (void)guid;
  (void)data;
  (void)data_size;
  (void)flags;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_texture_GetPrivateData(IDirect3DTexture9 *iface,
                                                    REFGUID guid,
                                                    void *data,
                                                    DWORD *data_size) {
  (void)iface;
  (void)guid;
  (void)data;
  (void)data_size;
  return D3DERR_NOTFOUND;
}

static HRESULT WINAPI dx9mt_texture_FreePrivateData(IDirect3DTexture9 *iface,
                                                     REFGUID guid) {
  (void)iface;
  (void)guid;
  return D3D_OK;
}

static DWORD WINAPI dx9mt_texture_SetPriority(IDirect3DTexture9 *iface,
                                              DWORD priority_new) {
  (void)iface;
  (void)priority_new;
  return 0;
}

static DWORD WINAPI dx9mt_texture_GetPriority(IDirect3DTexture9 *iface) {
  (void)iface;
  return 0;
}

static void WINAPI dx9mt_texture_PreLoad(IDirect3DTexture9 *iface) {
  (void)iface;
}

static D3DRESOURCETYPE WINAPI dx9mt_texture_GetType(IDirect3DTexture9 *iface) {
  (void)iface;
  return D3DRTYPE_TEXTURE;
}

static DWORD WINAPI dx9mt_texture_SetLOD(IDirect3DTexture9 *iface,
                                         DWORD lod_new) {
  dx9mt_texture *self = dx9mt_texture_from_iface(iface);
  DWORD old_lod = self->lod;
  if (lod_new < self->levels) {
    self->lod = lod_new;
  }
  return old_lod;
}

static DWORD WINAPI dx9mt_texture_GetLOD(IDirect3DTexture9 *iface) {
  dx9mt_texture *self = dx9mt_texture_from_iface(iface);
  return self->lod;
}

static DWORD WINAPI dx9mt_texture_GetLevelCount(IDirect3DTexture9 *iface) {
  dx9mt_texture *self = dx9mt_texture_from_iface(iface);
  return self->levels;
}

static HRESULT WINAPI dx9mt_texture_SetAutoGenFilterType(
    IDirect3DTexture9 *iface, D3DTEXTUREFILTERTYPE filter_type) {
  dx9mt_texture *self = dx9mt_texture_from_iface(iface);
  self->autogen_filter = filter_type;
  return D3D_OK;
}

static D3DTEXTUREFILTERTYPE WINAPI dx9mt_texture_GetAutoGenFilterType(
    IDirect3DTexture9 *iface) {
  dx9mt_texture *self = dx9mt_texture_from_iface(iface);
  return self->autogen_filter;
}

static void WINAPI dx9mt_texture_GenerateMipSubLevels(IDirect3DTexture9 *iface) {
  (void)iface;
}

static HRESULT WINAPI dx9mt_texture_GetLevelDesc(IDirect3DTexture9 *iface,
                                                  UINT level,
                                                  D3DSURFACE_DESC *desc) {
  dx9mt_texture *self = dx9mt_texture_from_iface(iface);
  if (!desc || level >= self->levels) {
    return D3DERR_INVALIDCALL;
  }
  return IDirect3DSurface9_GetDesc(self->surfaces[level], desc);
}

static HRESULT WINAPI dx9mt_texture_GetSurfaceLevel(IDirect3DTexture9 *iface,
                                                     UINT level,
                                                     IDirect3DSurface9 **surface) {
  dx9mt_texture *self = dx9mt_texture_from_iface(iface);
  if (!surface || level >= self->levels) {
    return D3DERR_INVALIDCALL;
  }

  *surface = self->surfaces[level];
  IDirect3DSurface9_AddRef(*surface);
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_texture_LockRect(IDirect3DTexture9 *iface,
                                              UINT level,
                                              D3DLOCKED_RECT *locked_rect,
                                              const RECT *rect, DWORD flags) {
  dx9mt_texture *self = dx9mt_texture_from_iface(iface);
  if (level >= self->levels) {
    return D3DERR_INVALIDCALL;
  }
  return IDirect3DSurface9_LockRect(self->surfaces[level], locked_rect, rect,
                                    flags);
}

static HRESULT WINAPI dx9mt_texture_UnlockRect(IDirect3DTexture9 *iface,
                                                UINT level) {
  dx9mt_texture *self = dx9mt_texture_from_iface(iface);
  if (level >= self->levels) {
    return D3DERR_INVALIDCALL;
  }
  return IDirect3DSurface9_UnlockRect(self->surfaces[level]);
}

static HRESULT WINAPI dx9mt_texture_AddDirtyRect(IDirect3DTexture9 *iface,
                                                  const RECT *dirty_rect) {
  dx9mt_texture *self = dx9mt_texture_from_iface(iface);
  (void)dirty_rect;
  dx9mt_texture_mark_dirty(self);
  return D3D_OK;
}

/* -------------------------------------------------------------------------- */
/* IDirect3DCubeTexture9                                                      */
/* -------------------------------------------------------------------------- */

static HRESULT WINAPI dx9mt_cube_texture_QueryInterface(
    IDirect3DCubeTexture9 *iface, REFIID riid, void **ppv_object);
static ULONG WINAPI dx9mt_cube_texture_AddRef(IDirect3DCubeTexture9 *iface);
static ULONG WINAPI dx9mt_cube_texture_Release(IDirect3DCubeTexture9 *iface);
static HRESULT WINAPI dx9mt_cube_texture_GetDevice(
    IDirect3DCubeTexture9 *iface, IDirect3DDevice9 **pp_device);
static HRESULT WINAPI dx9mt_cube_texture_SetPrivateData(
    IDirect3DCubeTexture9 *iface, REFGUID guid, const void *data,
    DWORD data_size, DWORD flags);
static HRESULT WINAPI dx9mt_cube_texture_GetPrivateData(
    IDirect3DCubeTexture9 *iface, REFGUID guid, void *data, DWORD *data_size);
static HRESULT WINAPI dx9mt_cube_texture_FreePrivateData(
    IDirect3DCubeTexture9 *iface, REFGUID guid);
static DWORD WINAPI dx9mt_cube_texture_SetPriority(IDirect3DCubeTexture9 *iface,
                                                   DWORD priority_new);
static DWORD WINAPI dx9mt_cube_texture_GetPriority(IDirect3DCubeTexture9 *iface);
static void WINAPI dx9mt_cube_texture_PreLoad(IDirect3DCubeTexture9 *iface);
static D3DRESOURCETYPE WINAPI dx9mt_cube_texture_GetType(
    IDirect3DCubeTexture9 *iface);
static DWORD WINAPI dx9mt_cube_texture_SetLOD(IDirect3DCubeTexture9 *iface,
                                              DWORD lod_new);
static DWORD WINAPI dx9mt_cube_texture_GetLOD(IDirect3DCubeTexture9 *iface);
static DWORD WINAPI dx9mt_cube_texture_GetLevelCount(
    IDirect3DCubeTexture9 *iface);
static HRESULT WINAPI dx9mt_cube_texture_SetAutoGenFilterType(
    IDirect3DCubeTexture9 *iface, D3DTEXTUREFILTERTYPE filter_type);
static D3DTEXTUREFILTERTYPE WINAPI dx9mt_cube_texture_GetAutoGenFilterType(
    IDirect3DCubeTexture9 *iface);
static void WINAPI dx9mt_cube_texture_GenerateMipSubLevels(
    IDirect3DCubeTexture9 *iface);
static HRESULT WINAPI dx9mt_cube_texture_GetLevelDesc(
    IDirect3DCubeTexture9 *iface, UINT level, D3DSURFACE_DESC *desc);
static HRESULT WINAPI dx9mt_cube_texture_GetCubeMapSurface(
    IDirect3DCubeTexture9 *iface, D3DCUBEMAP_FACES face_type, UINT level,
    IDirect3DSurface9 **surface);
static HRESULT WINAPI dx9mt_cube_texture_LockRect(
    IDirect3DCubeTexture9 *iface, D3DCUBEMAP_FACES face_type, UINT level,
    D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags);
static HRESULT WINAPI dx9mt_cube_texture_UnlockRect(
    IDirect3DCubeTexture9 *iface, D3DCUBEMAP_FACES face_type, UINT level);
static HRESULT WINAPI dx9mt_cube_texture_AddDirtyRect(
    IDirect3DCubeTexture9 *iface, D3DCUBEMAP_FACES face_type,
    const RECT *dirty_rect);

static IDirect3DCubeTexture9Vtbl g_dx9mt_cube_texture_vtbl = {
    dx9mt_cube_texture_QueryInterface,
    dx9mt_cube_texture_AddRef,
    dx9mt_cube_texture_Release,
    dx9mt_cube_texture_GetDevice,
    dx9mt_cube_texture_SetPrivateData,
    dx9mt_cube_texture_GetPrivateData,
    dx9mt_cube_texture_FreePrivateData,
    dx9mt_cube_texture_SetPriority,
    dx9mt_cube_texture_GetPriority,
    dx9mt_cube_texture_PreLoad,
    dx9mt_cube_texture_GetType,
    dx9mt_cube_texture_SetLOD,
    dx9mt_cube_texture_GetLOD,
    dx9mt_cube_texture_GetLevelCount,
    dx9mt_cube_texture_SetAutoGenFilterType,
    dx9mt_cube_texture_GetAutoGenFilterType,
    dx9mt_cube_texture_GenerateMipSubLevels,
    dx9mt_cube_texture_GetLevelDesc,
    dx9mt_cube_texture_GetCubeMapSurface,
    dx9mt_cube_texture_LockRect,
    dx9mt_cube_texture_UnlockRect,
    dx9mt_cube_texture_AddDirtyRect,
};

static WINBOOL dx9mt_cube_face_valid(D3DCUBEMAP_FACES face_type) {
  return face_type >= D3DCUBEMAP_FACE_POSITIVE_X &&
         face_type <= D3DCUBEMAP_FACE_NEGATIVE_Z;
}

static UINT dx9mt_cube_surface_index(UINT levels, D3DCUBEMAP_FACES face_type,
                                     UINT level) {
  return ((UINT)face_type * levels) + level;
}

static HRESULT dx9mt_cube_texture_create(dx9mt_device *device, UINT edge_length,
                                         UINT levels, DWORD usage,
                                         D3DFORMAT format, D3DPOOL pool,
                                         IDirect3DCubeTexture9 **out_cube) {
  dx9mt_cube_texture *cube;
  UINT face;
  UINT level;
  UINT level_edge;
  WINBOOL lockable;

  if (!out_cube || edge_length == 0) {
    return D3DERR_INVALIDCALL;
  }
  *out_cube = NULL;

  if (levels == 0) {
    levels = 1;
  }

  cube = (dx9mt_cube_texture *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                         sizeof(dx9mt_cube_texture));
  if (!cube) {
    return E_OUTOFMEMORY;
  }

  cube->surfaces = (IDirect3DSurface9 **)HeapAlloc(
      GetProcessHeap(), HEAP_ZERO_MEMORY,
      (SIZE_T)(levels * 6) * sizeof(IDirect3DSurface9 *));
  if (!cube->surfaces) {
    HeapFree(GetProcessHeap(), 0, cube);
    return E_OUTOFMEMORY;
  }

  cube->iface.lpVtbl = &g_dx9mt_cube_texture_vtbl;
  cube->refcount = 1;
  cube->object_id = dx9mt_alloc_object_id(DX9MT_OBJECT_KIND_TEXTURE);
  cube->device = device;
  cube->usage = usage;
  cube->format = format;
  cube->pool = pool;
  cube->edge_length = edge_length;
  cube->levels = levels;
  cube->autogen_filter = D3DTEXF_LINEAR;
  cube->generation = 1;

  lockable = ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) == 0);

  for (face = 0; face < 6; ++face) {
    level_edge = edge_length;
    for (level = 0; level < levels; ++level) {
      UINT index = dx9mt_cube_surface_index(levels, (D3DCUBEMAP_FACES)face, level);
      HRESULT hr = dx9mt_surface_create(
          device, level_edge, level_edge, format, pool, usage,
          D3DMULTISAMPLE_NONE, 0, lockable, (IUnknown *)&cube->iface,
          &cube->surfaces[index]);
      if (FAILED(hr)) {
        UINT cleanup;
        for (cleanup = 0; cleanup < (levels * 6); ++cleanup) {
          if (cube->surfaces[cleanup]) {
            IDirect3DSurface9_Release(cube->surfaces[cleanup]);
          }
        }
        HeapFree(GetProcessHeap(), 0, cube->surfaces);
        HeapFree(GetProcessHeap(), 0, cube);
        return hr;
      }

      if (level_edge > 1) {
        level_edge /= 2;
      }
    }
  }

  *out_cube = &cube->iface;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_cube_texture_QueryInterface(
    IDirect3DCubeTexture9 *iface, REFIID riid, void **ppv_object) {
  if (!ppv_object) {
    return E_POINTER;
  }

  if (IsEqualGUID(riid, &IID_IUnknown) ||
      IsEqualGUID(riid, &IID_IDirect3DResource9) ||
      IsEqualGUID(riid, &IID_IDirect3DBaseTexture9) ||
      IsEqualGUID(riid, &IID_IDirect3DCubeTexture9)) {
    *ppv_object = iface;
    dx9mt_cube_texture_AddRef(iface);
    return S_OK;
  }

  *ppv_object = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI dx9mt_cube_texture_AddRef(IDirect3DCubeTexture9 *iface) {
  dx9mt_cube_texture *self = dx9mt_cube_texture_from_iface(iface);
  return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG WINAPI dx9mt_cube_texture_Release(IDirect3DCubeTexture9 *iface) {
  dx9mt_cube_texture *self = dx9mt_cube_texture_from_iface(iface);
  LONG refcount = InterlockedDecrement(&self->refcount);
  UINT i;

  if (refcount == 0) {
    for (i = 0; i < (self->levels * 6); ++i) {
      if (self->surfaces[i]) {
        dx9mt_surface *surface = dx9mt_surface_from_iface(self->surfaces[i]);
        surface->container = NULL;
        IDirect3DSurface9_Release(self->surfaces[i]);
      }
    }
    HeapFree(GetProcessHeap(), 0, self->surfaces);
    HeapFree(GetProcessHeap(), 0, self);
  }

  return (ULONG)refcount;
}

static HRESULT WINAPI dx9mt_cube_texture_GetDevice(
    IDirect3DCubeTexture9 *iface, IDirect3DDevice9 **pp_device) {
  dx9mt_cube_texture *self = dx9mt_cube_texture_from_iface(iface);
  if (!pp_device) {
    return D3DERR_INVALIDCALL;
  }

  *pp_device = self->device ? &self->device->iface : NULL;
  if (*pp_device) {
    IDirect3DDevice9_AddRef(*pp_device);
  }
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_cube_texture_SetPrivateData(
    IDirect3DCubeTexture9 *iface, REFGUID guid, const void *data,
    DWORD data_size, DWORD flags) {
  (void)iface;
  (void)guid;
  (void)data;
  (void)data_size;
  (void)flags;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_cube_texture_GetPrivateData(
    IDirect3DCubeTexture9 *iface, REFGUID guid, void *data, DWORD *data_size) {
  (void)iface;
  (void)guid;
  (void)data;
  (void)data_size;
  return D3DERR_NOTFOUND;
}

static HRESULT WINAPI dx9mt_cube_texture_FreePrivateData(
    IDirect3DCubeTexture9 *iface, REFGUID guid) {
  (void)iface;
  (void)guid;
  return D3D_OK;
}

static DWORD WINAPI dx9mt_cube_texture_SetPriority(IDirect3DCubeTexture9 *iface,
                                                   DWORD priority_new) {
  (void)iface;
  (void)priority_new;
  return 0;
}

static DWORD WINAPI dx9mt_cube_texture_GetPriority(IDirect3DCubeTexture9 *iface) {
  (void)iface;
  return 0;
}

static void WINAPI dx9mt_cube_texture_PreLoad(IDirect3DCubeTexture9 *iface) {
  (void)iface;
}

static D3DRESOURCETYPE WINAPI dx9mt_cube_texture_GetType(
    IDirect3DCubeTexture9 *iface) {
  (void)iface;
  return D3DRTYPE_CUBETEXTURE;
}

static DWORD WINAPI dx9mt_cube_texture_SetLOD(IDirect3DCubeTexture9 *iface,
                                              DWORD lod_new) {
  dx9mt_cube_texture *self = dx9mt_cube_texture_from_iface(iface);
  DWORD old_lod = self->lod;
  if (lod_new < self->levels) {
    self->lod = lod_new;
  }
  return old_lod;
}

static DWORD WINAPI dx9mt_cube_texture_GetLOD(IDirect3DCubeTexture9 *iface) {
  dx9mt_cube_texture *self = dx9mt_cube_texture_from_iface(iface);
  return self->lod;
}

static DWORD WINAPI dx9mt_cube_texture_GetLevelCount(
    IDirect3DCubeTexture9 *iface) {
  dx9mt_cube_texture *self = dx9mt_cube_texture_from_iface(iface);
  return self->levels;
}

static HRESULT WINAPI dx9mt_cube_texture_SetAutoGenFilterType(
    IDirect3DCubeTexture9 *iface, D3DTEXTUREFILTERTYPE filter_type) {
  dx9mt_cube_texture *self = dx9mt_cube_texture_from_iface(iface);
  self->autogen_filter = filter_type;
  return D3D_OK;
}

static D3DTEXTUREFILTERTYPE WINAPI dx9mt_cube_texture_GetAutoGenFilterType(
    IDirect3DCubeTexture9 *iface) {
  dx9mt_cube_texture *self = dx9mt_cube_texture_from_iface(iface);
  return self->autogen_filter;
}

static void WINAPI dx9mt_cube_texture_GenerateMipSubLevels(
    IDirect3DCubeTexture9 *iface) {
  (void)iface;
}

static HRESULT WINAPI dx9mt_cube_texture_GetLevelDesc(
    IDirect3DCubeTexture9 *iface, UINT level, D3DSURFACE_DESC *desc) {
  dx9mt_cube_texture *self = dx9mt_cube_texture_from_iface(iface);
  if (!desc || level >= self->levels) {
    return D3DERR_INVALIDCALL;
  }
  return IDirect3DSurface9_GetDesc(
      self->surfaces[dx9mt_cube_surface_index(self->levels,
                                              D3DCUBEMAP_FACE_POSITIVE_X,
                                              level)],
      desc);
}

static HRESULT WINAPI dx9mt_cube_texture_GetCubeMapSurface(
    IDirect3DCubeTexture9 *iface, D3DCUBEMAP_FACES face_type, UINT level,
    IDirect3DSurface9 **surface) {
  dx9mt_cube_texture *self = dx9mt_cube_texture_from_iface(iface);
  UINT index;

  if (!surface || !dx9mt_cube_face_valid(face_type) || level >= self->levels) {
    return D3DERR_INVALIDCALL;
  }

  index = dx9mt_cube_surface_index(self->levels, face_type, level);
  *surface = self->surfaces[index];
  IDirect3DSurface9_AddRef(*surface);
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_cube_texture_LockRect(
    IDirect3DCubeTexture9 *iface, D3DCUBEMAP_FACES face_type, UINT level,
    D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags) {
  dx9mt_cube_texture *self = dx9mt_cube_texture_from_iface(iface);
  if (!dx9mt_cube_face_valid(face_type) || level >= self->levels) {
    return D3DERR_INVALIDCALL;
  }
  return IDirect3DSurface9_LockRect(
      self->surfaces[dx9mt_cube_surface_index(self->levels, face_type, level)],
      locked_rect, rect, flags);
}

static HRESULT WINAPI dx9mt_cube_texture_UnlockRect(
    IDirect3DCubeTexture9 *iface, D3DCUBEMAP_FACES face_type, UINT level) {
  dx9mt_cube_texture *self = dx9mt_cube_texture_from_iface(iface);
  if (!dx9mt_cube_face_valid(face_type) || level >= self->levels) {
    return D3DERR_INVALIDCALL;
  }
  return IDirect3DSurface9_UnlockRect(
      self->surfaces[dx9mt_cube_surface_index(self->levels, face_type, level)]);
}

static HRESULT WINAPI dx9mt_cube_texture_AddDirtyRect(
    IDirect3DCubeTexture9 *iface, D3DCUBEMAP_FACES face_type,
    const RECT *dirty_rect) {
  dx9mt_cube_texture *self = dx9mt_cube_texture_from_iface(iface);
  (void)dirty_rect;
  if (!dx9mt_cube_face_valid(face_type)) {
    return D3DERR_INVALIDCALL;
  }
  dx9mt_cube_texture_mark_dirty(self);
  return D3D_OK;
}

/* -------------------------------------------------------------------------- */
/* Shared resource object helpers (VB, IB, declaration, shaders)             */
/* -------------------------------------------------------------------------- */

static HRESULT dx9mt_copy_shader_blob(const DWORD *src, DWORD **dst,
                                      UINT *dword_count) {
  UINT count;
  DWORD *blob;

  if (!src || !dst || !dword_count) {
    return D3DERR_INVALIDCALL;
  }

  count = dx9mt_shader_dword_count(src);
  if (!count) {
    return D3DERR_INVALIDCALL;
  }

  blob = (DWORD *)HeapAlloc(GetProcessHeap(), 0, count * sizeof(DWORD));
  if (!blob) {
    return E_OUTOFMEMORY;
  }

  memcpy(blob, src, count * sizeof(DWORD));
  *dst = blob;
  *dword_count = count;
  return D3D_OK;
}

/* IDirect3DVertexBuffer9 */
static HRESULT WINAPI dx9mt_vb_QueryInterface(IDirect3DVertexBuffer9 *iface,
                                               REFIID riid,
                                               void **ppv_object);
static ULONG WINAPI dx9mt_vb_AddRef(IDirect3DVertexBuffer9 *iface);
static ULONG WINAPI dx9mt_vb_Release(IDirect3DVertexBuffer9 *iface);
static HRESULT WINAPI dx9mt_vb_GetDevice(IDirect3DVertexBuffer9 *iface,
                                          IDirect3DDevice9 **pp_device);
static HRESULT WINAPI dx9mt_vb_SetPrivateData(IDirect3DVertexBuffer9 *iface,
                                               REFGUID guid,
                                               const void *data,
                                               DWORD data_size,
                                               DWORD flags);
static HRESULT WINAPI dx9mt_vb_GetPrivateData(IDirect3DVertexBuffer9 *iface,
                                               REFGUID guid,
                                               void *data,
                                               DWORD *data_size);
static HRESULT WINAPI dx9mt_vb_FreePrivateData(IDirect3DVertexBuffer9 *iface,
                                                REFGUID guid);
static DWORD WINAPI dx9mt_vb_SetPriority(IDirect3DVertexBuffer9 *iface,
                                         DWORD priority_new);
static DWORD WINAPI dx9mt_vb_GetPriority(IDirect3DVertexBuffer9 *iface);
static void WINAPI dx9mt_vb_PreLoad(IDirect3DVertexBuffer9 *iface);
static D3DRESOURCETYPE WINAPI dx9mt_vb_GetType(IDirect3DVertexBuffer9 *iface);
static HRESULT WINAPI dx9mt_vb_Lock(IDirect3DVertexBuffer9 *iface,
                                     UINT offset_to_lock, UINT size_to_lock,
                                     void **data, DWORD flags);
static HRESULT WINAPI dx9mt_vb_Unlock(IDirect3DVertexBuffer9 *iface);
static HRESULT WINAPI dx9mt_vb_GetDesc(IDirect3DVertexBuffer9 *iface,
                                        D3DVERTEXBUFFER_DESC *desc);

static IDirect3DVertexBuffer9Vtbl g_dx9mt_vb_vtbl = {
    dx9mt_vb_QueryInterface,
    dx9mt_vb_AddRef,
    dx9mt_vb_Release,
    dx9mt_vb_GetDevice,
    dx9mt_vb_SetPrivateData,
    dx9mt_vb_GetPrivateData,
    dx9mt_vb_FreePrivateData,
    dx9mt_vb_SetPriority,
    dx9mt_vb_GetPriority,
    dx9mt_vb_PreLoad,
    dx9mt_vb_GetType,
    dx9mt_vb_Lock,
    dx9mt_vb_Unlock,
    dx9mt_vb_GetDesc,
};

static HRESULT dx9mt_vb_create(dx9mt_device *device, UINT length, DWORD usage,
                               DWORD fvf, D3DPOOL pool,
                               IDirect3DVertexBuffer9 **out_vb) {
  dx9mt_vertex_buffer *vb;

  if (!out_vb) {
    return D3DERR_INVALIDCALL;
  }
  *out_vb = NULL;

  vb = (dx9mt_vertex_buffer *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                        sizeof(dx9mt_vertex_buffer));
  if (!vb) {
    return E_OUTOFMEMORY;
  }

  vb->iface.lpVtbl = &g_dx9mt_vb_vtbl;
  vb->refcount = 1;
  vb->object_id = dx9mt_alloc_object_id(DX9MT_OBJECT_KIND_BUFFER);
  vb->device = device;
  vb->desc.Format = D3DFMT_VERTEXDATA;
  vb->desc.Type = D3DRTYPE_VERTEXBUFFER;
  vb->desc.Usage = usage;
  vb->desc.Pool = pool;
  vb->desc.Size = length;
  vb->desc.FVF = fvf;

  vb->data = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, length);
  if (!vb->data) {
    HeapFree(GetProcessHeap(), 0, vb);
    return E_OUTOFMEMORY;
  }

  *out_vb = &vb->iface;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_vb_QueryInterface(IDirect3DVertexBuffer9 *iface,
                                               REFIID riid,
                                               void **ppv_object) {
  if (!ppv_object) {
    return E_POINTER;
  }
  if (IsEqualGUID(riid, &IID_IUnknown) ||
      IsEqualGUID(riid, &IID_IDirect3DResource9) ||
      IsEqualGUID(riid, &IID_IDirect3DVertexBuffer9)) {
    *ppv_object = iface;
    dx9mt_vb_AddRef(iface);
    return S_OK;
  }

  *ppv_object = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI dx9mt_vb_AddRef(IDirect3DVertexBuffer9 *iface) {
  dx9mt_vertex_buffer *self = dx9mt_vb_from_iface(iface);
  return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG WINAPI dx9mt_vb_Release(IDirect3DVertexBuffer9 *iface) {
  dx9mt_vertex_buffer *self = dx9mt_vb_from_iface(iface);
  LONG refcount = InterlockedDecrement(&self->refcount);

  if (refcount == 0) {
    HeapFree(GetProcessHeap(), 0, self->data);
    HeapFree(GetProcessHeap(), 0, self);
  }

  return (ULONG)refcount;
}

static HRESULT WINAPI dx9mt_vb_GetDevice(IDirect3DVertexBuffer9 *iface,
                                          IDirect3DDevice9 **pp_device) {
  dx9mt_vertex_buffer *self = dx9mt_vb_from_iface(iface);
  if (!pp_device) {
    return D3DERR_INVALIDCALL;
  }

  *pp_device = self->device ? &self->device->iface : NULL;
  if (*pp_device) {
    IDirect3DDevice9_AddRef(*pp_device);
  }
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_vb_SetPrivateData(IDirect3DVertexBuffer9 *iface,
                                               REFGUID guid,
                                               const void *data,
                                               DWORD data_size,
                                               DWORD flags) {
  (void)iface;
  (void)guid;
  (void)data;
  (void)data_size;
  (void)flags;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_vb_GetPrivateData(IDirect3DVertexBuffer9 *iface,
                                               REFGUID guid,
                                               void *data,
                                               DWORD *data_size) {
  (void)iface;
  (void)guid;
  (void)data;
  (void)data_size;
  return D3DERR_NOTFOUND;
}

static HRESULT WINAPI dx9mt_vb_FreePrivateData(IDirect3DVertexBuffer9 *iface,
                                                REFGUID guid) {
  (void)iface;
  (void)guid;
  return D3D_OK;
}

static DWORD WINAPI dx9mt_vb_SetPriority(IDirect3DVertexBuffer9 *iface,
                                         DWORD priority_new) {
  (void)iface;
  (void)priority_new;
  return 0;
}

static DWORD WINAPI dx9mt_vb_GetPriority(IDirect3DVertexBuffer9 *iface) {
  (void)iface;
  return 0;
}

static void WINAPI dx9mt_vb_PreLoad(IDirect3DVertexBuffer9 *iface) {
  (void)iface;
}

static D3DRESOURCETYPE WINAPI dx9mt_vb_GetType(IDirect3DVertexBuffer9 *iface) {
  (void)iface;
  return D3DRTYPE_VERTEXBUFFER;
}

static HRESULT WINAPI dx9mt_vb_Lock(IDirect3DVertexBuffer9 *iface,
                                     UINT offset_to_lock, UINT size_to_lock,
                                     void **data, DWORD flags) {
  dx9mt_vertex_buffer *self = dx9mt_vb_from_iface(iface);
  (void)flags;

  if (!data || offset_to_lock > self->desc.Size) {
    return D3DERR_INVALIDCALL;
  }

  if (size_to_lock == 0 || offset_to_lock + size_to_lock > self->desc.Size) {
    size_to_lock = self->desc.Size - offset_to_lock;
  }

  *data = self->data + offset_to_lock;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_vb_Unlock(IDirect3DVertexBuffer9 *iface) {
  (void)iface;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_vb_GetDesc(IDirect3DVertexBuffer9 *iface,
                                        D3DVERTEXBUFFER_DESC *desc) {
  dx9mt_vertex_buffer *self = dx9mt_vb_from_iface(iface);
  if (!desc) {
    return D3DERR_INVALIDCALL;
  }

  *desc = self->desc;
  return D3D_OK;
}

/* IDirect3DIndexBuffer9 */
static HRESULT WINAPI dx9mt_ib_QueryInterface(IDirect3DIndexBuffer9 *iface,
                                               REFIID riid,
                                               void **ppv_object);
static ULONG WINAPI dx9mt_ib_AddRef(IDirect3DIndexBuffer9 *iface);
static ULONG WINAPI dx9mt_ib_Release(IDirect3DIndexBuffer9 *iface);
static HRESULT WINAPI dx9mt_ib_GetDevice(IDirect3DIndexBuffer9 *iface,
                                          IDirect3DDevice9 **pp_device);
static HRESULT WINAPI dx9mt_ib_SetPrivateData(IDirect3DIndexBuffer9 *iface,
                                               REFGUID guid,
                                               const void *data,
                                               DWORD data_size,
                                               DWORD flags);
static HRESULT WINAPI dx9mt_ib_GetPrivateData(IDirect3DIndexBuffer9 *iface,
                                               REFGUID guid,
                                               void *data,
                                               DWORD *data_size);
static HRESULT WINAPI dx9mt_ib_FreePrivateData(IDirect3DIndexBuffer9 *iface,
                                                REFGUID guid);
static DWORD WINAPI dx9mt_ib_SetPriority(IDirect3DIndexBuffer9 *iface,
                                         DWORD priority_new);
static DWORD WINAPI dx9mt_ib_GetPriority(IDirect3DIndexBuffer9 *iface);
static void WINAPI dx9mt_ib_PreLoad(IDirect3DIndexBuffer9 *iface);
static D3DRESOURCETYPE WINAPI dx9mt_ib_GetType(IDirect3DIndexBuffer9 *iface);
static HRESULT WINAPI dx9mt_ib_Lock(IDirect3DIndexBuffer9 *iface,
                                     UINT offset_to_lock, UINT size_to_lock,
                                     void **data, DWORD flags);
static HRESULT WINAPI dx9mt_ib_Unlock(IDirect3DIndexBuffer9 *iface);
static HRESULT WINAPI dx9mt_ib_GetDesc(IDirect3DIndexBuffer9 *iface,
                                        D3DINDEXBUFFER_DESC *desc);

static IDirect3DIndexBuffer9Vtbl g_dx9mt_ib_vtbl = {
    dx9mt_ib_QueryInterface,  dx9mt_ib_AddRef,   dx9mt_ib_Release,
    dx9mt_ib_GetDevice,       dx9mt_ib_SetPrivateData,
    dx9mt_ib_GetPrivateData,  dx9mt_ib_FreePrivateData,
    dx9mt_ib_SetPriority,     dx9mt_ib_GetPriority,
    dx9mt_ib_PreLoad,         dx9mt_ib_GetType,
    dx9mt_ib_Lock,            dx9mt_ib_Unlock,
    dx9mt_ib_GetDesc,
};

static HRESULT dx9mt_ib_create(dx9mt_device *device, UINT length, DWORD usage,
                               D3DFORMAT format, D3DPOOL pool,
                               IDirect3DIndexBuffer9 **out_ib) {
  dx9mt_index_buffer *ib;

  if (!out_ib) {
    return D3DERR_INVALIDCALL;
  }
  *out_ib = NULL;

  ib = (dx9mt_index_buffer *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                       sizeof(dx9mt_index_buffer));
  if (!ib) {
    return E_OUTOFMEMORY;
  }

  ib->iface.lpVtbl = &g_dx9mt_ib_vtbl;
  ib->refcount = 1;
  ib->object_id = dx9mt_alloc_object_id(DX9MT_OBJECT_KIND_BUFFER);
  ib->device = device;
  ib->desc.Format = format;
  ib->desc.Type = D3DRTYPE_INDEXBUFFER;
  ib->desc.Usage = usage;
  ib->desc.Pool = pool;
  ib->desc.Size = length;

  ib->data = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, length);
  if (!ib->data) {
    HeapFree(GetProcessHeap(), 0, ib);
    return E_OUTOFMEMORY;
  }

  *out_ib = &ib->iface;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_ib_QueryInterface(IDirect3DIndexBuffer9 *iface,
                                               REFIID riid,
                                               void **ppv_object) {
  if (!ppv_object) {
    return E_POINTER;
  }
  if (IsEqualGUID(riid, &IID_IUnknown) ||
      IsEqualGUID(riid, &IID_IDirect3DResource9) ||
      IsEqualGUID(riid, &IID_IDirect3DIndexBuffer9)) {
    *ppv_object = iface;
    dx9mt_ib_AddRef(iface);
    return S_OK;
  }

  *ppv_object = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI dx9mt_ib_AddRef(IDirect3DIndexBuffer9 *iface) {
  dx9mt_index_buffer *self = dx9mt_ib_from_iface(iface);
  return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG WINAPI dx9mt_ib_Release(IDirect3DIndexBuffer9 *iface) {
  dx9mt_index_buffer *self = dx9mt_ib_from_iface(iface);
  LONG refcount = InterlockedDecrement(&self->refcount);

  if (refcount == 0) {
    HeapFree(GetProcessHeap(), 0, self->data);
    HeapFree(GetProcessHeap(), 0, self);
  }

  return (ULONG)refcount;
}

static HRESULT WINAPI dx9mt_ib_GetDevice(IDirect3DIndexBuffer9 *iface,
                                          IDirect3DDevice9 **pp_device) {
  dx9mt_index_buffer *self = dx9mt_ib_from_iface(iface);
  if (!pp_device) {
    return D3DERR_INVALIDCALL;
  }

  *pp_device = self->device ? &self->device->iface : NULL;
  if (*pp_device) {
    IDirect3DDevice9_AddRef(*pp_device);
  }
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_ib_SetPrivateData(IDirect3DIndexBuffer9 *iface,
                                               REFGUID guid,
                                               const void *data,
                                               DWORD data_size,
                                               DWORD flags) {
  (void)iface;
  (void)guid;
  (void)data;
  (void)data_size;
  (void)flags;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_ib_GetPrivateData(IDirect3DIndexBuffer9 *iface,
                                               REFGUID guid,
                                               void *data,
                                               DWORD *data_size) {
  (void)iface;
  (void)guid;
  (void)data;
  (void)data_size;
  return D3DERR_NOTFOUND;
}

static HRESULT WINAPI dx9mt_ib_FreePrivateData(IDirect3DIndexBuffer9 *iface,
                                                REFGUID guid) {
  (void)iface;
  (void)guid;
  return D3D_OK;
}

static DWORD WINAPI dx9mt_ib_SetPriority(IDirect3DIndexBuffer9 *iface,
                                         DWORD priority_new) {
  (void)iface;
  (void)priority_new;
  return 0;
}

static DWORD WINAPI dx9mt_ib_GetPriority(IDirect3DIndexBuffer9 *iface) {
  (void)iface;
  return 0;
}

static void WINAPI dx9mt_ib_PreLoad(IDirect3DIndexBuffer9 *iface) {
  (void)iface;
}

static D3DRESOURCETYPE WINAPI dx9mt_ib_GetType(IDirect3DIndexBuffer9 *iface) {
  (void)iface;
  return D3DRTYPE_INDEXBUFFER;
}

static HRESULT WINAPI dx9mt_ib_Lock(IDirect3DIndexBuffer9 *iface,
                                     UINT offset_to_lock, UINT size_to_lock,
                                     void **data, DWORD flags) {
  dx9mt_index_buffer *self = dx9mt_ib_from_iface(iface);
  (void)flags;

  if (!data || offset_to_lock > self->desc.Size) {
    return D3DERR_INVALIDCALL;
  }

  if (size_to_lock == 0 || offset_to_lock + size_to_lock > self->desc.Size) {
    size_to_lock = self->desc.Size - offset_to_lock;
  }

  *data = self->data + offset_to_lock;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_ib_Unlock(IDirect3DIndexBuffer9 *iface) {
  (void)iface;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_ib_GetDesc(IDirect3DIndexBuffer9 *iface,
                                        D3DINDEXBUFFER_DESC *desc) {
  dx9mt_index_buffer *self = dx9mt_ib_from_iface(iface);
  if (!desc) {
    return D3DERR_INVALIDCALL;
  }

  *desc = self->desc;
  return D3D_OK;
}

/* IDirect3DVertexDeclaration9 */
static HRESULT WINAPI dx9mt_vdecl_QueryInterface(IDirect3DVertexDeclaration9 *iface,
                                                  REFIID riid,
                                                  void **ppv_object);
static ULONG WINAPI dx9mt_vdecl_AddRef(IDirect3DVertexDeclaration9 *iface);
static ULONG WINAPI dx9mt_vdecl_Release(IDirect3DVertexDeclaration9 *iface);
static HRESULT WINAPI dx9mt_vdecl_GetDevice(IDirect3DVertexDeclaration9 *iface,
                                             IDirect3DDevice9 **pp_device);
static HRESULT WINAPI dx9mt_vdecl_GetDeclaration(IDirect3DVertexDeclaration9 *iface,
                                                  D3DVERTEXELEMENT9 *elements,
                                                  UINT *num_elements);

static IDirect3DVertexDeclaration9Vtbl g_dx9mt_vdecl_vtbl = {
    dx9mt_vdecl_QueryInterface,
    dx9mt_vdecl_AddRef,
    dx9mt_vdecl_Release,
    dx9mt_vdecl_GetDevice,
    dx9mt_vdecl_GetDeclaration,
};

static HRESULT dx9mt_vdecl_create(dx9mt_device *device,
                                  const D3DVERTEXELEMENT9 *elements,
                                  IDirect3DVertexDeclaration9 **out_decl) {
  dx9mt_vertex_decl *decl;
  UINT count = 0;

  if (!elements || !out_decl) {
    return D3DERR_INVALIDCALL;
  }
  *out_decl = NULL;

  while (!(elements[count].Stream == 0xFF && elements[count].Type == D3DDECLTYPE_UNUSED)) {
    ++count;
    if (count > 128) {
      return D3DERR_INVALIDCALL;
    }
  }
  ++count;

  decl = (dx9mt_vertex_decl *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                        sizeof(dx9mt_vertex_decl));
  if (!decl) {
    return E_OUTOFMEMORY;
  }

  decl->iface.lpVtbl = &g_dx9mt_vdecl_vtbl;
  decl->refcount = 1;
  decl->object_id = dx9mt_alloc_object_id(DX9MT_OBJECT_KIND_VERTEX_DECL);
  decl->device = device;
  decl->count = count;
  decl->elements = (D3DVERTEXELEMENT9 *)HeapAlloc(GetProcessHeap(), 0,
                                                  sizeof(D3DVERTEXELEMENT9) * count);
  if (!decl->elements) {
    HeapFree(GetProcessHeap(), 0, decl);
    return E_OUTOFMEMORY;
  }

  memcpy(decl->elements, elements, sizeof(D3DVERTEXELEMENT9) * count);
  *out_decl = &decl->iface;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_vdecl_QueryInterface(IDirect3DVertexDeclaration9 *iface,
                                                  REFIID riid,
                                                  void **ppv_object) {
  if (!ppv_object) {
    return E_POINTER;
  }

  if (IsEqualGUID(riid, &IID_IUnknown) ||
      IsEqualGUID(riid, &IID_IDirect3DVertexDeclaration9)) {
    *ppv_object = iface;
    dx9mt_vdecl_AddRef(iface);
    return S_OK;
  }

  *ppv_object = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI dx9mt_vdecl_AddRef(IDirect3DVertexDeclaration9 *iface) {
  dx9mt_vertex_decl *self = dx9mt_vdecl_from_iface(iface);
  return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG WINAPI dx9mt_vdecl_Release(IDirect3DVertexDeclaration9 *iface) {
  dx9mt_vertex_decl *self = dx9mt_vdecl_from_iface(iface);
  LONG refcount = InterlockedDecrement(&self->refcount);
  if (refcount == 0) {
    HeapFree(GetProcessHeap(), 0, self->elements);
    HeapFree(GetProcessHeap(), 0, self);
  }
  return (ULONG)refcount;
}

static HRESULT WINAPI dx9mt_vdecl_GetDevice(IDirect3DVertexDeclaration9 *iface,
                                             IDirect3DDevice9 **pp_device) {
  dx9mt_vertex_decl *self = dx9mt_vdecl_from_iface(iface);
  if (!pp_device) {
    return D3DERR_INVALIDCALL;
  }

  *pp_device = self->device ? &self->device->iface : NULL;
  if (*pp_device) {
    IDirect3DDevice9_AddRef(*pp_device);
  }
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_vdecl_GetDeclaration(IDirect3DVertexDeclaration9 *iface,
                                                  D3DVERTEXELEMENT9 *elements,
                                                  UINT *num_elements) {
  dx9mt_vertex_decl *self = dx9mt_vdecl_from_iface(iface);
  UINT bytes;

  if (!num_elements) {
    return D3DERR_INVALIDCALL;
  }

  bytes = self->count * (UINT)sizeof(D3DVERTEXELEMENT9);
  if (!elements) {
    *num_elements = bytes;
    return D3D_OK;
  }

  if (*num_elements < bytes) {
    return D3DERR_INVALIDCALL;
  }

  memcpy(elements, self->elements, bytes);
  *num_elements = bytes;
  return D3D_OK;
}

/* IDirect3DVertexShader9 */
static HRESULT WINAPI dx9mt_vshader_QueryInterface(IDirect3DVertexShader9 *iface,
                                                    REFIID riid,
                                                    void **ppv_object);
static ULONG WINAPI dx9mt_vshader_AddRef(IDirect3DVertexShader9 *iface);
static ULONG WINAPI dx9mt_vshader_Release(IDirect3DVertexShader9 *iface);
static HRESULT WINAPI dx9mt_vshader_GetDevice(IDirect3DVertexShader9 *iface,
                                               IDirect3DDevice9 **pp_device);
static HRESULT WINAPI dx9mt_vshader_GetFunction(IDirect3DVertexShader9 *iface,
                                                 void *data, UINT *data_size);

static IDirect3DVertexShader9Vtbl g_dx9mt_vshader_vtbl = {
    dx9mt_vshader_QueryInterface,
    dx9mt_vshader_AddRef,
    dx9mt_vshader_Release,
    dx9mt_vshader_GetDevice,
    dx9mt_vshader_GetFunction,
};

static HRESULT dx9mt_vshader_create(dx9mt_device *device, const DWORD *byte_code,
                                    IDirect3DVertexShader9 **out_shader) {
  dx9mt_vertex_shader *shader;
  HRESULT hr;

  if (!out_shader) {
    return D3DERR_INVALIDCALL;
  }
  *out_shader = NULL;

  shader = (dx9mt_vertex_shader *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                            sizeof(dx9mt_vertex_shader));
  if (!shader) {
    return E_OUTOFMEMORY;
  }

  shader->iface.lpVtbl = &g_dx9mt_vshader_vtbl;
  shader->refcount = 1;
  shader->object_id = dx9mt_alloc_object_id(DX9MT_OBJECT_KIND_VERTEX_SHADER);
  shader->device = device;

  hr = dx9mt_copy_shader_blob(byte_code, &shader->byte_code,
                              &shader->dword_count);
  if (FAILED(hr)) {
    HeapFree(GetProcessHeap(), 0, shader);
    return hr;
  }

  *out_shader = &shader->iface;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_vshader_QueryInterface(IDirect3DVertexShader9 *iface,
                                                    REFIID riid,
                                                    void **ppv_object) {
  if (!ppv_object) {
    return E_POINTER;
  }

  if (IsEqualGUID(riid, &IID_IUnknown) ||
      IsEqualGUID(riid, &IID_IDirect3DVertexShader9)) {
    *ppv_object = iface;
    dx9mt_vshader_AddRef(iface);
    return S_OK;
  }

  *ppv_object = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI dx9mt_vshader_AddRef(IDirect3DVertexShader9 *iface) {
  dx9mt_vertex_shader *self = dx9mt_vshader_from_iface(iface);
  return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG WINAPI dx9mt_vshader_Release(IDirect3DVertexShader9 *iface) {
  dx9mt_vertex_shader *self = dx9mt_vshader_from_iface(iface);
  LONG refcount = InterlockedDecrement(&self->refcount);
  if (refcount == 0) {
    HeapFree(GetProcessHeap(), 0, self->byte_code);
    HeapFree(GetProcessHeap(), 0, self);
  }
  return (ULONG)refcount;
}

static HRESULT WINAPI dx9mt_vshader_GetDevice(IDirect3DVertexShader9 *iface,
                                               IDirect3DDevice9 **pp_device) {
  dx9mt_vertex_shader *self = dx9mt_vshader_from_iface(iface);
  if (!pp_device) {
    return D3DERR_INVALIDCALL;
  }

  *pp_device = self->device ? &self->device->iface : NULL;
  if (*pp_device) {
    IDirect3DDevice9_AddRef(*pp_device);
  }
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_vshader_GetFunction(IDirect3DVertexShader9 *iface,
                                                 void *data, UINT *data_size) {
  dx9mt_vertex_shader *self = dx9mt_vshader_from_iface(iface);
  UINT bytes;

  if (!data_size) {
    return D3DERR_INVALIDCALL;
  }

  bytes = self->dword_count * (UINT)sizeof(DWORD);
  if (!data) {
    *data_size = bytes;
    return D3D_OK;
  }

  if (*data_size < bytes) {
    return D3DERR_INVALIDCALL;
  }

  memcpy(data, self->byte_code, bytes);
  *data_size = bytes;
  return D3D_OK;
}

/* IDirect3DPixelShader9 */
static HRESULT WINAPI dx9mt_pshader_QueryInterface(IDirect3DPixelShader9 *iface,
                                                    REFIID riid,
                                                    void **ppv_object);
static ULONG WINAPI dx9mt_pshader_AddRef(IDirect3DPixelShader9 *iface);
static ULONG WINAPI dx9mt_pshader_Release(IDirect3DPixelShader9 *iface);
static HRESULT WINAPI dx9mt_pshader_GetDevice(IDirect3DPixelShader9 *iface,
                                               IDirect3DDevice9 **pp_device);
static HRESULT WINAPI dx9mt_pshader_GetFunction(IDirect3DPixelShader9 *iface,
                                                 void *data, UINT *data_size);

static IDirect3DPixelShader9Vtbl g_dx9mt_pshader_vtbl = {
    dx9mt_pshader_QueryInterface,
    dx9mt_pshader_AddRef,
    dx9mt_pshader_Release,
    dx9mt_pshader_GetDevice,
    dx9mt_pshader_GetFunction,
};

static HRESULT dx9mt_pshader_create(dx9mt_device *device, const DWORD *byte_code,
                                    IDirect3DPixelShader9 **out_shader) {
  dx9mt_pixel_shader *shader;
  HRESULT hr;

  if (!out_shader) {
    return D3DERR_INVALIDCALL;
  }
  *out_shader = NULL;

  shader = (dx9mt_pixel_shader *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                           sizeof(dx9mt_pixel_shader));
  if (!shader) {
    return E_OUTOFMEMORY;
  }

  shader->iface.lpVtbl = &g_dx9mt_pshader_vtbl;
  shader->refcount = 1;
  shader->object_id = dx9mt_alloc_object_id(DX9MT_OBJECT_KIND_PIXEL_SHADER);
  shader->device = device;

  hr = dx9mt_copy_shader_blob(byte_code, &shader->byte_code,
                              &shader->dword_count);
  if (FAILED(hr)) {
    HeapFree(GetProcessHeap(), 0, shader);
    return hr;
  }

  *out_shader = &shader->iface;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_pshader_QueryInterface(IDirect3DPixelShader9 *iface,
                                                    REFIID riid,
                                                    void **ppv_object) {
  if (!ppv_object) {
    return E_POINTER;
  }

  if (IsEqualGUID(riid, &IID_IUnknown) ||
      IsEqualGUID(riid, &IID_IDirect3DPixelShader9)) {
    *ppv_object = iface;
    dx9mt_pshader_AddRef(iface);
    return S_OK;
  }

  *ppv_object = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI dx9mt_pshader_AddRef(IDirect3DPixelShader9 *iface) {
  dx9mt_pixel_shader *self = dx9mt_pshader_from_iface(iface);
  return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG WINAPI dx9mt_pshader_Release(IDirect3DPixelShader9 *iface) {
  dx9mt_pixel_shader *self = dx9mt_pshader_from_iface(iface);
  LONG refcount = InterlockedDecrement(&self->refcount);
  if (refcount == 0) {
    HeapFree(GetProcessHeap(), 0, self->byte_code);
    HeapFree(GetProcessHeap(), 0, self);
  }
  return (ULONG)refcount;
}

static HRESULT WINAPI dx9mt_pshader_GetDevice(IDirect3DPixelShader9 *iface,
                                               IDirect3DDevice9 **pp_device) {
  dx9mt_pixel_shader *self = dx9mt_pshader_from_iface(iface);
  if (!pp_device) {
    return D3DERR_INVALIDCALL;
  }

  *pp_device = self->device ? &self->device->iface : NULL;
  if (*pp_device) {
    IDirect3DDevice9_AddRef(*pp_device);
  }
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_pshader_GetFunction(IDirect3DPixelShader9 *iface,
                                                 void *data, UINT *data_size) {
  dx9mt_pixel_shader *self = dx9mt_pshader_from_iface(iface);
  UINT bytes;

  if (!data_size) {
    return D3DERR_INVALIDCALL;
  }

  bytes = self->dword_count * (UINT)sizeof(DWORD);
  if (!data) {
    *data_size = bytes;
    return D3D_OK;
  }

  if (*data_size < bytes) {
    return D3DERR_INVALIDCALL;
  }

  memcpy(data, self->byte_code, bytes);
  *data_size = bytes;
  return D3D_OK;
}

/* -------------------------------------------------------------------------- */
/* IDirect3DQuery9 implementation                                             */
/* -------------------------------------------------------------------------- */

static DWORD dx9mt_query_data_size(D3DQUERYTYPE type) {
  switch (type) {
  case D3DQUERYTYPE_EVENT:
    return sizeof(WINBOOL);
  case D3DQUERYTYPE_OCCLUSION:
    return sizeof(DWORD);
  case D3DQUERYTYPE_TIMESTAMP:
  case D3DQUERYTYPE_TIMESTAMPDISJOINT:
  case D3DQUERYTYPE_TIMESTAMPFREQ:
    return sizeof(UINT64);
  default:
    return 0;
  }
}

static HRESULT WINAPI dx9mt_query_QueryInterface(IDirect3DQuery9 *iface,
                                                 REFIID riid,
                                                 void **ppv_object) {
  if (!ppv_object) {
    return E_POINTER;
  }

  if (IsEqualGUID(riid, &IID_IUnknown) ||
      IsEqualGUID(riid, &IID_IDirect3DQuery9)) {
    *ppv_object = iface;
    IDirect3DQuery9_AddRef(iface);
    return S_OK;
  }

  *ppv_object = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI dx9mt_query_AddRef(IDirect3DQuery9 *iface) {
  dx9mt_query *self = dx9mt_query_from_iface(iface);
  return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG WINAPI dx9mt_query_Release(IDirect3DQuery9 *iface) {
  dx9mt_query *self = dx9mt_query_from_iface(iface);
  LONG refcount = InterlockedDecrement(&self->refcount);

  if (refcount == 0) {
    dx9mt_safe_release((IUnknown *)self->device);
    HeapFree(GetProcessHeap(), 0, self);
  }

  return (ULONG)refcount;
}

static HRESULT WINAPI dx9mt_query_GetDevice(IDirect3DQuery9 *iface,
                                            IDirect3DDevice9 **device) {
  dx9mt_query *self = dx9mt_query_from_iface(iface);

  if (!device) {
    return D3DERR_INVALIDCALL;
  }

  *device = self->device ? &self->device->iface : NULL;
  if (*device) {
    IDirect3DDevice9_AddRef(*device);
  }

  return D3D_OK;
}

static D3DQUERYTYPE WINAPI dx9mt_query_GetType(IDirect3DQuery9 *iface) {
  dx9mt_query *self = dx9mt_query_from_iface(iface);
  return self->type;
}

static DWORD WINAPI dx9mt_query_GetDataSize(IDirect3DQuery9 *iface) {
  dx9mt_query *self = dx9mt_query_from_iface(iface);
  return self->data_size;
}

static HRESULT WINAPI dx9mt_query_Issue(IDirect3DQuery9 *iface, DWORD issue_flags) {
  dx9mt_query *self = dx9mt_query_from_iface(iface);
  self->issued = TRUE;
  self->issue_flags = issue_flags;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_query_GetData(IDirect3DQuery9 *iface, void *data,
                                          DWORD size, DWORD get_data_flags) {
  dx9mt_query *self = dx9mt_query_from_iface(iface);

  (void)get_data_flags;

  if (!self->issued) {
    return S_FALSE;
  }

  if (size != 0 && !data) {
    return D3DERR_INVALIDCALL;
  }

  if (self->data_size != 0 && size < self->data_size) {
    return D3DERR_INVALIDCALL;
  }

  if (data && self->data_size) {
    memset(data, 0, self->data_size);

    if (self->type == D3DQUERYTYPE_EVENT) {
      *(WINBOOL *)data = TRUE;
    } else if (self->type == D3DQUERYTYPE_OCCLUSION) {
      *(DWORD *)data = 1;
    }
  }

  return D3D_OK;
}

static const IDirect3DQuery9Vtbl g_dx9mt_query_vtbl = {
    dx9mt_query_QueryInterface, dx9mt_query_AddRef,     dx9mt_query_Release,
    dx9mt_query_GetDevice,     dx9mt_query_GetType,     dx9mt_query_GetDataSize,
    dx9mt_query_Issue,         dx9mt_query_GetData,
};

/* -------------------------------------------------------------------------- */
/* IDirect3DDevice9 defaults + overrides                                      */
/* -------------------------------------------------------------------------- */

#define DX9MT_RETVAL_HRESULT do { return D3DERR_NOTAVAILABLE; } while (0)
#define DX9MT_RETVAL_ULONG do { return 1; } while (0)
#define DX9MT_RETVAL_UINT do { return 0; } while (0)
#define DX9MT_RETVAL_WINBOOL do { return TRUE; } while (0)
#define DX9MT_RETVAL_float do { return 0.0f; } while (0)
#define DX9MT_RETVAL_void do { return; } while (0)

#define DX9MT_DEVICE_METHOD(ret, name, ...)                                      \
  static ret WINAPI dx9mt_device_default_##name(IDirect3DDevice9 *iface,         \
                                                 ##__VA_ARGS__) {                 \
    dx9mt_logf("STUB", "%s (default stub hit)", #name);                          \
    (void)iface;                                                                  \
    DX9MT_RETVAL_##ret;                                                           \
  }
#include "d3d9_device_methods.inc"
#undef DX9MT_DEVICE_METHOD

#undef DX9MT_RETVAL_HRESULT
#undef DX9MT_RETVAL_ULONG
#undef DX9MT_RETVAL_UINT
#undef DX9MT_RETVAL_WINBOOL
#undef DX9MT_RETVAL_float
#undef DX9MT_RETVAL_void

/* custom method declarations */
static HRESULT WINAPI dx9mt_device_QueryInterface(IDirect3DDevice9 *iface,
                                                   REFIID riid,
                                                   void **ppv_object);
static ULONG WINAPI dx9mt_device_AddRef(IDirect3DDevice9 *iface);
static ULONG WINAPI dx9mt_device_Release(IDirect3DDevice9 *iface);
static HRESULT WINAPI dx9mt_device_TestCooperativeLevel(IDirect3DDevice9 *iface);
static UINT WINAPI dx9mt_device_GetAvailableTextureMem(IDirect3DDevice9 *iface);
static HRESULT WINAPI dx9mt_device_GetDirect3D(IDirect3DDevice9 *iface,
                                                IDirect3D9 **pp_d3d9);
static HRESULT WINAPI dx9mt_device_GetDeviceCaps(IDirect3DDevice9 *iface,
                                                  D3DCAPS9 *caps);
static HRESULT WINAPI dx9mt_device_GetDisplayMode(IDirect3DDevice9 *iface,
                                                   UINT swapchain_idx,
                                                   D3DDISPLAYMODE *mode);
static HRESULT WINAPI dx9mt_device_GetCreationParameters(
    IDirect3DDevice9 *iface, D3DDEVICE_CREATION_PARAMETERS *params);
static HRESULT WINAPI dx9mt_device_GetSwapChain(IDirect3DDevice9 *iface,
                                                 UINT swapchain_idx,
                                                 IDirect3DSwapChain9 **swapchain);
static UINT WINAPI dx9mt_device_GetNumberOfSwapChains(IDirect3DDevice9 *iface);
static HRESULT WINAPI dx9mt_device_Reset(IDirect3DDevice9 *iface,
                                          D3DPRESENT_PARAMETERS *params);
static HRESULT WINAPI dx9mt_device_Present(IDirect3DDevice9 *iface,
                                            const RECT *src_rect,
                                            const RECT *dst_rect,
                                            HWND dst_window_override,
                                            const RGNDATA *dirty_region);
static HRESULT WINAPI dx9mt_device_GetBackBuffer(IDirect3DDevice9 *iface,
                                                  UINT swapchain_idx,
                                                  UINT backbuffer_idx,
                                                  D3DBACKBUFFER_TYPE type,
                                                  IDirect3DSurface9 **surface);
static HRESULT WINAPI dx9mt_device_GetRasterStatus(IDirect3DDevice9 *iface,
                                                    UINT swapchain_idx,
                                                    D3DRASTER_STATUS *status);
static HRESULT WINAPI dx9mt_device_SetDialogBoxMode(IDirect3DDevice9 *iface,
                                                     WINBOOL enable);
static void WINAPI dx9mt_device_SetGammaRamp(IDirect3DDevice9 *iface,
                                              UINT swapchain_idx, DWORD flags,
                                              const D3DGAMMARAMP *ramp);
static void WINAPI dx9mt_device_GetGammaRamp(IDirect3DDevice9 *iface,
                                              UINT swapchain_idx,
                                              D3DGAMMARAMP *ramp);
static HRESULT WINAPI dx9mt_device_CreateVertexBuffer(
    IDirect3DDevice9 *iface, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
    IDirect3DVertexBuffer9 **vb, HANDLE *shared_handle);
static HRESULT WINAPI dx9mt_device_CreateTexture(
    IDirect3DDevice9 *iface, UINT width, UINT height, UINT levels, DWORD usage,
    D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9 **texture,
    HANDLE *shared_handle);
static HRESULT WINAPI dx9mt_device_CreateVolumeTexture(
    IDirect3DDevice9 *iface, UINT width, UINT height, UINT depth, UINT levels,
    DWORD usage, D3DFORMAT format, D3DPOOL pool,
    IDirect3DVolumeTexture9 **volume_texture, HANDLE *shared_handle);
static HRESULT WINAPI dx9mt_device_CreateCubeTexture(
    IDirect3DDevice9 *iface, UINT edge_length, UINT levels, DWORD usage,
    D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9 **cube_texture,
    HANDLE *shared_handle);
static HRESULT WINAPI dx9mt_device_CreateIndexBuffer(
    IDirect3DDevice9 *iface, UINT length, DWORD usage, D3DFORMAT format,
    D3DPOOL pool, IDirect3DIndexBuffer9 **ib, HANDLE *shared_handle);
static HRESULT WINAPI dx9mt_device_CreateRenderTarget(
    IDirect3DDevice9 *iface, UINT width, UINT height, D3DFORMAT format,
    D3DMULTISAMPLE_TYPE multisample, DWORD quality, WINBOOL lockable,
    IDirect3DSurface9 **surface, HANDLE *shared_handle);
static HRESULT WINAPI dx9mt_device_CreateDepthStencilSurface(
    IDirect3DDevice9 *iface, UINT width, UINT height, D3DFORMAT format,
    D3DMULTISAMPLE_TYPE multisample, DWORD quality, WINBOOL discard,
    IDirect3DSurface9 **surface, HANDLE *shared_handle);
static HRESULT WINAPI dx9mt_device_CreateOffscreenPlainSurface(
    IDirect3DDevice9 *iface, UINT width, UINT height, D3DFORMAT format,
    D3DPOOL pool, IDirect3DSurface9 **surface, HANDLE *shared_handle);
static HRESULT WINAPI dx9mt_device_UpdateSurface(
    IDirect3DDevice9 *iface, IDirect3DSurface9 *src_surface,
    const RECT *src_rect, IDirect3DSurface9 *dst_surface,
    const POINT *dst_point);
static HRESULT WINAPI dx9mt_device_UpdateTexture(
    IDirect3DDevice9 *iface, IDirect3DBaseTexture9 *src_texture,
    IDirect3DBaseTexture9 *dst_texture);
static HRESULT WINAPI dx9mt_device_GetRenderTargetData(
    IDirect3DDevice9 *iface, IDirect3DSurface9 *render_target,
    IDirect3DSurface9 *dest_surface);
static HRESULT WINAPI dx9mt_device_GetFrontBufferData(
    IDirect3DDevice9 *iface, UINT swapchain_idx, IDirect3DSurface9 *dest_surface);
static HRESULT WINAPI dx9mt_device_StretchRect(
    IDirect3DDevice9 *iface, IDirect3DSurface9 *src_surface,
    const RECT *src_rect, IDirect3DSurface9 *dst_surface, const RECT *dst_rect,
    D3DTEXTUREFILTERTYPE filter);
static HRESULT WINAPI dx9mt_device_ColorFill(IDirect3DDevice9 *iface,
                                              IDirect3DSurface9 *surface,
                                              const RECT *rect, D3DCOLOR color);
static HRESULT WINAPI dx9mt_device_SetRenderTarget(IDirect3DDevice9 *iface,
                                                    DWORD index,
                                                    IDirect3DSurface9 *surface);
static HRESULT WINAPI dx9mt_device_GetRenderTarget(IDirect3DDevice9 *iface,
                                                    DWORD index,
                                                    IDirect3DSurface9 **surface);
static HRESULT WINAPI dx9mt_device_SetDepthStencilSurface(
    IDirect3DDevice9 *iface, IDirect3DSurface9 *surface);
static HRESULT WINAPI dx9mt_device_GetDepthStencilSurface(
    IDirect3DDevice9 *iface, IDirect3DSurface9 **surface);
static HRESULT WINAPI dx9mt_device_BeginScene(IDirect3DDevice9 *iface);
static HRESULT WINAPI dx9mt_device_EndScene(IDirect3DDevice9 *iface);
static HRESULT WINAPI dx9mt_device_Clear(IDirect3DDevice9 *iface,
                                          DWORD rect_count,
                                          const D3DRECT *rects, DWORD flags,
                                          D3DCOLOR color, float z,
                                          DWORD stencil);
static HRESULT WINAPI dx9mt_device_SetTransform(IDirect3DDevice9 *iface,
                                                 D3DTRANSFORMSTATETYPE state,
                                                 const D3DMATRIX *matrix);
static HRESULT WINAPI dx9mt_device_GetTransform(IDirect3DDevice9 *iface,
                                                 D3DTRANSFORMSTATETYPE state,
                                                 D3DMATRIX *matrix);
static HRESULT WINAPI dx9mt_device_SetViewport(IDirect3DDevice9 *iface,
                                                const D3DVIEWPORT9 *viewport);
static HRESULT WINAPI dx9mt_device_GetViewport(IDirect3DDevice9 *iface,
                                                D3DVIEWPORT9 *viewport);
static HRESULT WINAPI dx9mt_device_SetClipPlane(IDirect3DDevice9 *iface,
                                                 DWORD index,
                                                 const float *plane);
static HRESULT WINAPI dx9mt_device_GetClipPlane(IDirect3DDevice9 *iface,
                                                 DWORD index, float *plane);
static HRESULT WINAPI dx9mt_device_SetRenderState(IDirect3DDevice9 *iface,
                                                   D3DRENDERSTATETYPE state,
                                                   DWORD value);
static HRESULT WINAPI dx9mt_device_GetRenderState(IDirect3DDevice9 *iface,
                                                   D3DRENDERSTATETYPE state,
                                                   DWORD *value);
static HRESULT WINAPI dx9mt_device_SetTexture(IDirect3DDevice9 *iface,
                                               DWORD stage,
                                               IDirect3DBaseTexture9 *texture);
static HRESULT WINAPI dx9mt_device_GetTexture(IDirect3DDevice9 *iface,
                                               DWORD stage,
                                               IDirect3DBaseTexture9 **texture);
static HRESULT WINAPI dx9mt_device_SetTextureStageState(
    IDirect3DDevice9 *iface, DWORD stage, D3DTEXTURESTAGESTATETYPE type,
    DWORD value);
static HRESULT WINAPI dx9mt_device_GetTextureStageState(
    IDirect3DDevice9 *iface, DWORD stage, D3DTEXTURESTAGESTATETYPE type,
    DWORD *value);
static HRESULT WINAPI dx9mt_device_SetSamplerState(IDirect3DDevice9 *iface,
                                                    DWORD sampler,
                                                    D3DSAMPLERSTATETYPE type,
                                                    DWORD value);
static HRESULT WINAPI dx9mt_device_GetSamplerState(IDirect3DDevice9 *iface,
                                                    DWORD sampler,
                                                    D3DSAMPLERSTATETYPE type,
                                                    DWORD *value);
static HRESULT WINAPI dx9mt_device_SetScissorRect(IDirect3DDevice9 *iface,
                                                   const RECT *rect);
static HRESULT WINAPI dx9mt_device_GetScissorRect(IDirect3DDevice9 *iface,
                                                   RECT *rect);
static HRESULT WINAPI dx9mt_device_SetSoftwareVertexProcessing(
    IDirect3DDevice9 *iface, WINBOOL software);
static WINBOOL WINAPI dx9mt_device_GetSoftwareVertexProcessing(
    IDirect3DDevice9 *iface);
static HRESULT WINAPI dx9mt_device_SetNPatchMode(IDirect3DDevice9 *iface,
                                                  float n_segments);
static float WINAPI dx9mt_device_GetNPatchMode(IDirect3DDevice9 *iface);
static HRESULT WINAPI dx9mt_device_DrawPrimitive(IDirect3DDevice9 *iface,
                                                  D3DPRIMITIVETYPE primitive_type,
                                                  UINT start_vertex,
                                                  UINT primitive_count);
static HRESULT WINAPI dx9mt_device_DrawIndexedPrimitive(
    IDirect3DDevice9 *iface, D3DPRIMITIVETYPE primitive_type,
    INT base_vertex_index, UINT min_vertex_index, UINT num_vertices,
    UINT start_index, UINT prim_count);
static HRESULT WINAPI dx9mt_device_CreateVertexDeclaration(
    IDirect3DDevice9 *iface, const D3DVERTEXELEMENT9 *elements,
    IDirect3DVertexDeclaration9 **declaration);
static HRESULT WINAPI dx9mt_device_SetVertexDeclaration(
    IDirect3DDevice9 *iface, IDirect3DVertexDeclaration9 *decl);
static HRESULT WINAPI dx9mt_device_GetVertexDeclaration(
    IDirect3DDevice9 *iface, IDirect3DVertexDeclaration9 **decl);
static HRESULT WINAPI dx9mt_device_SetFVF(IDirect3DDevice9 *iface, DWORD fvf);
static HRESULT WINAPI dx9mt_device_GetFVF(IDirect3DDevice9 *iface, DWORD *fvf);
static HRESULT WINAPI dx9mt_device_CreateVertexShader(
    IDirect3DDevice9 *iface, const DWORD *byte_code,
    IDirect3DVertexShader9 **shader);
static HRESULT WINAPI dx9mt_device_SetVertexShader(IDirect3DDevice9 *iface,
                                                    IDirect3DVertexShader9 *shader);
static HRESULT WINAPI dx9mt_device_GetVertexShader(
    IDirect3DDevice9 *iface, IDirect3DVertexShader9 **shader);
static HRESULT WINAPI dx9mt_device_SetVertexShaderConstantF(
    IDirect3DDevice9 *iface, UINT reg_idx, const float *data, UINT count);
static HRESULT WINAPI dx9mt_device_GetVertexShaderConstantF(
    IDirect3DDevice9 *iface, UINT reg_idx, float *data, UINT count);
static HRESULT WINAPI dx9mt_device_SetVertexShaderConstantI(
    IDirect3DDevice9 *iface, UINT reg_idx, const int *data, UINT count);
static HRESULT WINAPI dx9mt_device_GetVertexShaderConstantI(
    IDirect3DDevice9 *iface, UINT reg_idx, int *data, UINT count);
static HRESULT WINAPI dx9mt_device_SetVertexShaderConstantB(
    IDirect3DDevice9 *iface, UINT reg_idx, const WINBOOL *data, UINT count);
static HRESULT WINAPI dx9mt_device_GetVertexShaderConstantB(
    IDirect3DDevice9 *iface, UINT reg_idx, WINBOOL *data, UINT count);
static HRESULT WINAPI dx9mt_device_SetStreamSource(
    IDirect3DDevice9 *iface, UINT stream_number,
    IDirect3DVertexBuffer9 *stream_data, UINT offset_in_bytes, UINT stride);
static HRESULT WINAPI dx9mt_device_GetStreamSource(
    IDirect3DDevice9 *iface, UINT stream_number,
    IDirect3DVertexBuffer9 **stream_data, UINT *offset_in_bytes, UINT *stride);
static HRESULT WINAPI dx9mt_device_SetStreamSourceFreq(IDirect3DDevice9 *iface,
                                                        UINT stream_number,
                                                        UINT divider);
static HRESULT WINAPI dx9mt_device_GetStreamSourceFreq(IDirect3DDevice9 *iface,
                                                        UINT stream_number,
                                                        UINT *divider);
static HRESULT WINAPI dx9mt_device_SetIndices(IDirect3DDevice9 *iface,
                                               IDirect3DIndexBuffer9 *index_data);
static HRESULT WINAPI dx9mt_device_GetIndices(IDirect3DDevice9 *iface,
                                               IDirect3DIndexBuffer9 **index_data);
static HRESULT WINAPI dx9mt_device_CreatePixelShader(
    IDirect3DDevice9 *iface, const DWORD *byte_code,
    IDirect3DPixelShader9 **shader);
static HRESULT WINAPI dx9mt_device_SetPixelShader(IDirect3DDevice9 *iface,
                                                   IDirect3DPixelShader9 *shader);
static HRESULT WINAPI dx9mt_device_GetPixelShader(IDirect3DDevice9 *iface,
                                                   IDirect3DPixelShader9 **shader);
static HRESULT WINAPI dx9mt_device_SetPixelShaderConstantF(
    IDirect3DDevice9 *iface, UINT reg_idx, const float *data, UINT count);
static HRESULT WINAPI dx9mt_device_GetPixelShaderConstantF(
    IDirect3DDevice9 *iface, UINT reg_idx, float *data, UINT count);
static HRESULT WINAPI dx9mt_device_SetPixelShaderConstantI(
    IDirect3DDevice9 *iface, UINT reg_idx, const int *data, UINT count);
static HRESULT WINAPI dx9mt_device_GetPixelShaderConstantI(
    IDirect3DDevice9 *iface, UINT reg_idx, int *data, UINT count);
static HRESULT WINAPI dx9mt_device_SetPixelShaderConstantB(
    IDirect3DDevice9 *iface, UINT reg_idx, const WINBOOL *data, UINT count);
static HRESULT WINAPI dx9mt_device_GetPixelShaderConstantB(
    IDirect3DDevice9 *iface, UINT reg_idx, WINBOOL *data, UINT count);
static HRESULT WINAPI dx9mt_device_CreateQuery(IDirect3DDevice9 *iface,
                                                D3DQUERYTYPE type,
                                                IDirect3DQuery9 **query);

static IDirect3DDevice9Vtbl g_dx9mt_device_default_vtbl = {
#define DX9MT_DEVICE_METHOD(ret, name, ...) .name = dx9mt_device_default_##name,
#include "d3d9_device_methods.inc"
#undef DX9MT_DEVICE_METHOD
};

static IDirect3DDevice9Vtbl g_dx9mt_device_vtbl;
static LONG g_dx9mt_device_vtbl_state;

static void dx9mt_device_init_vtbl(void) {
  LONG state = InterlockedCompareExchange(&g_dx9mt_device_vtbl_state, 1, 0);

  if (state == 2) {
    return;
  }

  if (state == 1) {
    while (InterlockedCompareExchange(&g_dx9mt_device_vtbl_state, 2, 2) != 2) {
      Sleep(0);
    }
    return;
  }

  g_dx9mt_device_vtbl = g_dx9mt_device_default_vtbl;

  g_dx9mt_device_vtbl.QueryInterface = dx9mt_device_QueryInterface;
  g_dx9mt_device_vtbl.AddRef = dx9mt_device_AddRef;
  g_dx9mt_device_vtbl.Release = dx9mt_device_Release;
  g_dx9mt_device_vtbl.TestCooperativeLevel = dx9mt_device_TestCooperativeLevel;
  g_dx9mt_device_vtbl.GetAvailableTextureMem = dx9mt_device_GetAvailableTextureMem;
  g_dx9mt_device_vtbl.GetDirect3D = dx9mt_device_GetDirect3D;
  g_dx9mt_device_vtbl.GetDeviceCaps = dx9mt_device_GetDeviceCaps;
  g_dx9mt_device_vtbl.GetDisplayMode = dx9mt_device_GetDisplayMode;
  g_dx9mt_device_vtbl.GetCreationParameters = dx9mt_device_GetCreationParameters;
  g_dx9mt_device_vtbl.GetSwapChain = dx9mt_device_GetSwapChain;
  g_dx9mt_device_vtbl.GetNumberOfSwapChains = dx9mt_device_GetNumberOfSwapChains;
  g_dx9mt_device_vtbl.Reset = dx9mt_device_Reset;
  g_dx9mt_device_vtbl.Present = dx9mt_device_Present;
  g_dx9mt_device_vtbl.GetBackBuffer = dx9mt_device_GetBackBuffer;
  g_dx9mt_device_vtbl.GetRasterStatus = dx9mt_device_GetRasterStatus;
  g_dx9mt_device_vtbl.SetDialogBoxMode = dx9mt_device_SetDialogBoxMode;
  g_dx9mt_device_vtbl.SetGammaRamp = dx9mt_device_SetGammaRamp;
  g_dx9mt_device_vtbl.GetGammaRamp = dx9mt_device_GetGammaRamp;
  g_dx9mt_device_vtbl.CreateTexture = dx9mt_device_CreateTexture;
  g_dx9mt_device_vtbl.CreateVolumeTexture = dx9mt_device_CreateVolumeTexture;
  g_dx9mt_device_vtbl.CreateCubeTexture = dx9mt_device_CreateCubeTexture;
  g_dx9mt_device_vtbl.CreateVertexBuffer = dx9mt_device_CreateVertexBuffer;
  g_dx9mt_device_vtbl.CreateIndexBuffer = dx9mt_device_CreateIndexBuffer;
  g_dx9mt_device_vtbl.CreateRenderTarget = dx9mt_device_CreateRenderTarget;
  g_dx9mt_device_vtbl.CreateDepthStencilSurface =
      dx9mt_device_CreateDepthStencilSurface;
  g_dx9mt_device_vtbl.CreateOffscreenPlainSurface =
      dx9mt_device_CreateOffscreenPlainSurface;
  g_dx9mt_device_vtbl.UpdateSurface = dx9mt_device_UpdateSurface;
  g_dx9mt_device_vtbl.UpdateTexture = dx9mt_device_UpdateTexture;
  g_dx9mt_device_vtbl.GetRenderTargetData = dx9mt_device_GetRenderTargetData;
  g_dx9mt_device_vtbl.GetFrontBufferData = dx9mt_device_GetFrontBufferData;
  g_dx9mt_device_vtbl.StretchRect = dx9mt_device_StretchRect;
  g_dx9mt_device_vtbl.ColorFill = dx9mt_device_ColorFill;
  g_dx9mt_device_vtbl.SetRenderTarget = dx9mt_device_SetRenderTarget;
  g_dx9mt_device_vtbl.GetRenderTarget = dx9mt_device_GetRenderTarget;
  g_dx9mt_device_vtbl.SetDepthStencilSurface = dx9mt_device_SetDepthStencilSurface;
  g_dx9mt_device_vtbl.GetDepthStencilSurface = dx9mt_device_GetDepthStencilSurface;
  g_dx9mt_device_vtbl.BeginScene = dx9mt_device_BeginScene;
  g_dx9mt_device_vtbl.EndScene = dx9mt_device_EndScene;
  g_dx9mt_device_vtbl.Clear = dx9mt_device_Clear;
  g_dx9mt_device_vtbl.SetTransform = dx9mt_device_SetTransform;
  g_dx9mt_device_vtbl.GetTransform = dx9mt_device_GetTransform;
  g_dx9mt_device_vtbl.SetViewport = dx9mt_device_SetViewport;
  g_dx9mt_device_vtbl.GetViewport = dx9mt_device_GetViewport;
  g_dx9mt_device_vtbl.SetClipPlane = dx9mt_device_SetClipPlane;
  g_dx9mt_device_vtbl.GetClipPlane = dx9mt_device_GetClipPlane;
  g_dx9mt_device_vtbl.SetRenderState = dx9mt_device_SetRenderState;
  g_dx9mt_device_vtbl.GetRenderState = dx9mt_device_GetRenderState;
  g_dx9mt_device_vtbl.SetTexture = dx9mt_device_SetTexture;
  g_dx9mt_device_vtbl.GetTexture = dx9mt_device_GetTexture;
  g_dx9mt_device_vtbl.SetTextureStageState = dx9mt_device_SetTextureStageState;
  g_dx9mt_device_vtbl.GetTextureStageState = dx9mt_device_GetTextureStageState;
  g_dx9mt_device_vtbl.SetSamplerState = dx9mt_device_SetSamplerState;
  g_dx9mt_device_vtbl.GetSamplerState = dx9mt_device_GetSamplerState;
  g_dx9mt_device_vtbl.SetScissorRect = dx9mt_device_SetScissorRect;
  g_dx9mt_device_vtbl.GetScissorRect = dx9mt_device_GetScissorRect;
  g_dx9mt_device_vtbl.SetSoftwareVertexProcessing =
      dx9mt_device_SetSoftwareVertexProcessing;
  g_dx9mt_device_vtbl.GetSoftwareVertexProcessing =
      dx9mt_device_GetSoftwareVertexProcessing;
  g_dx9mt_device_vtbl.SetNPatchMode = dx9mt_device_SetNPatchMode;
  g_dx9mt_device_vtbl.GetNPatchMode = dx9mt_device_GetNPatchMode;
  g_dx9mt_device_vtbl.DrawPrimitive = dx9mt_device_DrawPrimitive;
  g_dx9mt_device_vtbl.DrawIndexedPrimitive = dx9mt_device_DrawIndexedPrimitive;
  g_dx9mt_device_vtbl.CreateVertexDeclaration =
      dx9mt_device_CreateVertexDeclaration;
  g_dx9mt_device_vtbl.SetVertexDeclaration = dx9mt_device_SetVertexDeclaration;
  g_dx9mt_device_vtbl.GetVertexDeclaration = dx9mt_device_GetVertexDeclaration;
  g_dx9mt_device_vtbl.SetFVF = dx9mt_device_SetFVF;
  g_dx9mt_device_vtbl.GetFVF = dx9mt_device_GetFVF;
  g_dx9mt_device_vtbl.CreateVertexShader = dx9mt_device_CreateVertexShader;
  g_dx9mt_device_vtbl.SetVertexShader = dx9mt_device_SetVertexShader;
  g_dx9mt_device_vtbl.GetVertexShader = dx9mt_device_GetVertexShader;
  g_dx9mt_device_vtbl.SetVertexShaderConstantF =
      dx9mt_device_SetVertexShaderConstantF;
  g_dx9mt_device_vtbl.GetVertexShaderConstantF =
      dx9mt_device_GetVertexShaderConstantF;
  g_dx9mt_device_vtbl.SetVertexShaderConstantI =
      dx9mt_device_SetVertexShaderConstantI;
  g_dx9mt_device_vtbl.GetVertexShaderConstantI =
      dx9mt_device_GetVertexShaderConstantI;
  g_dx9mt_device_vtbl.SetVertexShaderConstantB =
      dx9mt_device_SetVertexShaderConstantB;
  g_dx9mt_device_vtbl.GetVertexShaderConstantB =
      dx9mt_device_GetVertexShaderConstantB;
  g_dx9mt_device_vtbl.SetStreamSource = dx9mt_device_SetStreamSource;
  g_dx9mt_device_vtbl.GetStreamSource = dx9mt_device_GetStreamSource;
  g_dx9mt_device_vtbl.SetStreamSourceFreq = dx9mt_device_SetStreamSourceFreq;
  g_dx9mt_device_vtbl.GetStreamSourceFreq = dx9mt_device_GetStreamSourceFreq;
  g_dx9mt_device_vtbl.SetIndices = dx9mt_device_SetIndices;
  g_dx9mt_device_vtbl.GetIndices = dx9mt_device_GetIndices;
  g_dx9mt_device_vtbl.CreatePixelShader = dx9mt_device_CreatePixelShader;
  g_dx9mt_device_vtbl.SetPixelShader = dx9mt_device_SetPixelShader;
  g_dx9mt_device_vtbl.GetPixelShader = dx9mt_device_GetPixelShader;
  g_dx9mt_device_vtbl.SetPixelShaderConstantF =
      dx9mt_device_SetPixelShaderConstantF;
  g_dx9mt_device_vtbl.GetPixelShaderConstantF =
      dx9mt_device_GetPixelShaderConstantF;
  g_dx9mt_device_vtbl.SetPixelShaderConstantI =
      dx9mt_device_SetPixelShaderConstantI;
  g_dx9mt_device_vtbl.GetPixelShaderConstantI =
      dx9mt_device_GetPixelShaderConstantI;
  g_dx9mt_device_vtbl.SetPixelShaderConstantB =
      dx9mt_device_SetPixelShaderConstantB;
  g_dx9mt_device_vtbl.GetPixelShaderConstantB =
      dx9mt_device_GetPixelShaderConstantB;
  g_dx9mt_device_vtbl.CreateQuery = dx9mt_device_CreateQuery;

  InterlockedExchange(&g_dx9mt_device_vtbl_state, 2);
}

static void dx9mt_device_init_default_states(dx9mt_device *self) {
  uint32_t stage;
  uint32_t sampler;

  if (!self) {
    return;
  }

  for (sampler = 0; sampler < DX9MT_MAX_SAMPLERS; ++sampler) {
    self->sampler_states[sampler][D3DSAMP_ADDRESSU] = D3DTADDRESS_WRAP;
    self->sampler_states[sampler][D3DSAMP_ADDRESSV] = D3DTADDRESS_WRAP;
    self->sampler_states[sampler][D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
    self->sampler_states[sampler][D3DSAMP_MAGFILTER] = D3DTEXF_POINT;
    self->sampler_states[sampler][D3DSAMP_MINFILTER] = D3DTEXF_POINT;
    self->sampler_states[sampler][D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
    self->sampler_states[sampler][D3DSAMP_MAXANISOTROPY] = 1;
  }

  for (stage = 0; stage < DX9MT_MAX_TEXTURE_STAGES; ++stage) {
    self->tex_stage_states[stage][D3DTSS_COLOROP] =
        (stage == 0) ? D3DTOP_MODULATE : D3DTOP_DISABLE;
    self->tex_stage_states[stage][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
    self->tex_stage_states[stage][D3DTSS_COLORARG2] = D3DTA_CURRENT;
    self->tex_stage_states[stage][D3DTSS_ALPHAOP] =
        (stage == 0) ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
    self->tex_stage_states[stage][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    self->tex_stage_states[stage][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
    self->tex_stage_states[stage][D3DTSS_RESULTARG] = D3DTA_CURRENT;
    self->tex_stage_states[stage][D3DTSS_TEXCOORDINDEX] = stage;
  }

  self->render_states[D3DRS_ZENABLE] = D3DZB_TRUE;
  self->render_states[D3DRS_ZWRITEENABLE] = TRUE;
  self->render_states[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
  self->render_states[D3DRS_ALPHABLENDENABLE] = FALSE;
  self->render_states[D3DRS_SRCBLEND] = D3DBLEND_ONE;
  self->render_states[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
  self->render_states[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
  self->render_states[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFFu;
  self->render_states[D3DRS_ALPHATESTENABLE] = FALSE;
  self->render_states[D3DRS_ALPHAREF] = 0;
  self->render_states[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
  self->render_states[D3DRS_STENCILENABLE] = FALSE;
  self->render_states[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
  self->render_states[D3DRS_STENCILREF] = 0;
  self->render_states[D3DRS_STENCILMASK] = 0xFFFFFFFFu;
  self->render_states[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFFu;
  self->render_states[D3DRS_CULLMODE] = D3DCULL_CCW;
  self->render_states[D3DRS_SCISSORTESTENABLE] = FALSE;
  self->render_states[D3DRS_COLORWRITEENABLE] = 0xF; /* all channels */
  self->render_states[D3DRS_STENCILPASS] = 1; /* D3DSTENCILOP_KEEP */
  self->render_states[D3DRS_STENCILFAIL] = 1; /* D3DSTENCILOP_KEEP */
  self->render_states[D3DRS_STENCILZFAIL] = 1; /* D3DSTENCILOP_KEEP */
  self->render_states[D3DRS_FOGENABLE] = FALSE;
  self->render_states[D3DRS_FOGCOLOR] = 0;
  *(float *)&self->render_states[D3DRS_FOGSTART] = 0.0f;
  *(float *)&self->render_states[D3DRS_FOGEND] = 1.0f;
  *(float *)&self->render_states[D3DRS_FOGDENSITY] = 1.0f;
  self->render_states[D3DRS_FOGTABLEMODE] = 0; /* D3DFOG_NONE */
}

static void dx9mt_device_release_bindings(dx9mt_device *self) {
  UINT i;

  for (i = 0; i < DX9MT_MAX_RENDER_TARGETS; ++i) {
    dx9mt_safe_release((IUnknown *)self->render_targets[i]);
    self->render_targets[i] = NULL;
  }

  dx9mt_safe_release((IUnknown *)self->depth_stencil);
  self->depth_stencil = NULL;

  for (i = 0; i < DX9MT_MAX_TEXTURE_STAGES; ++i) {
    dx9mt_safe_release((IUnknown *)self->textures[i]);
    self->textures[i] = NULL;
  }

  for (i = 0; i < DX9MT_MAX_STREAMS; ++i) {
    dx9mt_safe_release((IUnknown *)self->streams[i]);
    self->streams[i] = NULL;
  }

  dx9mt_safe_release((IUnknown *)self->indices);
  self->indices = NULL;

  dx9mt_safe_release((IUnknown *)self->vertex_decl);
  self->vertex_decl = NULL;
  dx9mt_safe_release((IUnknown *)self->vertex_shader);
  self->vertex_shader = NULL;
  dx9mt_safe_release((IUnknown *)self->pixel_shader);
  self->pixel_shader = NULL;
}

static HRESULT dx9mt_device_reset_internal(dx9mt_device *self,
                                           D3DPRESENT_PARAMETERS *params) {
  HRESULT hr;

  if (!params) {
    return D3DERR_INVALIDCALL;
  }

  dx9mt_device_release_bindings(self);

  if (self->swapchain) {
    IDirect3DSwapChain9_Release(&self->swapchain->iface);
    self->swapchain = NULL;
  }

  self->params = *params;

  hr = dx9mt_swapchain_create(self, &self->params, &self->swapchain);
  if (FAILED(hr)) {
    return hr;
  }

  self->render_targets[0] = self->swapchain->backbuffer;
  if (self->render_targets[0]) {
    IDirect3DSurface9_AddRef(self->render_targets[0]);
  }

  self->viewport.X = 0;
  self->viewport.Y = 0;
  self->viewport.Width = self->params.BackBufferWidth ? self->params.BackBufferWidth : 1280;
  self->viewport.Height = self->params.BackBufferHeight ? self->params.BackBufferHeight : 720;
  self->viewport.MinZ = 0.0f;
  self->viewport.MaxZ = 1.0f;

  self->scissor_rect.left = 0;
  self->scissor_rect.top = 0;
  self->scissor_rect.right = (LONG)self->viewport.Width;
  self->scissor_rect.bottom = (LONG)self->viewport.Height;

  self->present_target_id = self->swapchain ? self->swapchain->object_id : 0;
  hr = dx9mt_device_publish_present_target(self);
  if (FAILED(hr)) {
    return hr;
  }

  return D3D_OK;
}

/* custom device methods */
static HRESULT WINAPI dx9mt_device_QueryInterface(IDirect3DDevice9 *iface,
                                                   REFIID riid,
                                                   void **ppv_object) {
  if (!ppv_object) {
    return E_POINTER;
  }

  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3DDevice9)) {
    *ppv_object = iface;
    dx9mt_device_AddRef(iface);
    return S_OK;
  }

  *ppv_object = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI dx9mt_device_AddRef(IDirect3DDevice9 *iface) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG WINAPI dx9mt_device_Release(IDirect3DDevice9 *iface) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  LONG refcount = InterlockedDecrement(&self->refcount);

  if (refcount == 0) {
    dx9mt_device_release_bindings(self);

    if (self->swapchain) {
      IDirect3DSwapChain9_Release(&self->swapchain->iface);
      self->swapchain = NULL;
    }

    dx9mt_safe_release((IUnknown *)self->parent);
    HeapFree(GetProcessHeap(), 0, self);
  }

  return (ULONG)refcount;
}

static HRESULT WINAPI dx9mt_device_TestCooperativeLevel(IDirect3DDevice9 *iface) {
  (void)iface;
  return D3D_OK;
}

static UINT WINAPI dx9mt_device_GetAvailableTextureMem(IDirect3DDevice9 *iface) {
  (void)iface;
  return 512u * 1024u * 1024u;
}

static HRESULT WINAPI dx9mt_device_GetDirect3D(IDirect3DDevice9 *iface,
                                                IDirect3D9 **pp_d3d9) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!pp_d3d9) {
    return D3DERR_INVALIDCALL;
  }

  *pp_d3d9 = self->parent;
  if (*pp_d3d9) {
    IDirect3D9_AddRef(*pp_d3d9);
  }
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetDeviceCaps(IDirect3DDevice9 *iface,
                                                  D3DCAPS9 *caps) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!caps) {
    return D3DERR_INVALIDCALL;
  }
  return IDirect3D9_GetDeviceCaps(self->parent, self->adapter, self->device_type,
                                  caps);
}

static HRESULT WINAPI dx9mt_device_GetDisplayMode(IDirect3DDevice9 *iface,
                                                   UINT swapchain_idx,
                                                   D3DDISPLAYMODE *mode) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (swapchain_idx != 0 || !mode) {
    return D3DERR_INVALIDCALL;
  }

  return IDirect3DSwapChain9_GetDisplayMode(&self->swapchain->iface, mode);
}

static HRESULT WINAPI dx9mt_device_GetCreationParameters(
    IDirect3DDevice9 *iface, D3DDEVICE_CREATION_PARAMETERS *params) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!params) {
    return D3DERR_INVALIDCALL;
  }

  *params = self->creation;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetSwapChain(IDirect3DDevice9 *iface,
                                                 UINT swapchain_idx,
                                                 IDirect3DSwapChain9 **swapchain) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!swapchain || swapchain_idx != 0 || !self->swapchain) {
    return D3DERR_INVALIDCALL;
  }

  *swapchain = &self->swapchain->iface;
  IDirect3DSwapChain9_AddRef(*swapchain);
  return D3D_OK;
}

static UINT WINAPI dx9mt_device_GetNumberOfSwapChains(IDirect3DDevice9 *iface) {
  (void)iface;
  return 1;
}

static HRESULT WINAPI dx9mt_device_Reset(IDirect3DDevice9 *iface,
                                          D3DPRESENT_PARAMETERS *params) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  dx9mt_logf("device", "Reset called");
  return dx9mt_device_reset_internal(self, params);
}

static HRESULT WINAPI dx9mt_device_Present(IDirect3DDevice9 *iface,
                                            const RECT *src_rect,
                                            const RECT *dst_rect,
                                            HWND dst_window_override,
                                            const RGNDATA *dirty_region) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  HRESULT hr;
  dx9mt_packet_present packet;

  (void)src_rect;
  (void)dst_rect;
  (void)dirty_region;

  memset(&packet, 0, sizeof(packet));
  packet.header.type = DX9MT_PACKET_PRESENT;
  packet.header.size = (uint16_t)sizeof(packet);
  packet.header.sequence = dx9mt_runtime_next_packet_sequence();
  packet.frame_id = self->frame_id;
  packet.render_target_id =
      dx9mt_surface_object_id_from_iface(self->render_targets[0]);

  dx9mt_backend_bridge_submit_packets(&packet.header, (uint32_t)sizeof(packet));
  hr = dx9mt_backend_bridge_present(self->frame_id) == 0 ? D3D_OK : D3DERR_DEVICELOST;
  if (SUCCEEDED(hr)) {
    HRESULT soft_hr = dx9mt_device_soft_present(self, dst_window_override);
    if (FAILED(soft_hr)) {
      hr = soft_hr;
    }
  }

  ++self->frame_id;
  /* Invalidate cached constant refs -- arena slot rotates per frame */
  memset(&self->vs_const_last_ref, 0, sizeof(self->vs_const_last_ref));
  memset(&self->ps_const_last_ref, 0, sizeof(self->ps_const_last_ref));
  self->vs_const_dirty = TRUE;
  self->ps_const_dirty = TRUE;
  return hr;
}

static HRESULT WINAPI dx9mt_device_GetBackBuffer(IDirect3DDevice9 *iface,
                                                  UINT swapchain_idx,
                                                  UINT backbuffer_idx,
                                                  D3DBACKBUFFER_TYPE type,
                                                  IDirect3DSurface9 **surface) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (swapchain_idx != 0 || !surface) {
    return D3DERR_INVALIDCALL;
  }
  return IDirect3DSwapChain9_GetBackBuffer(&self->swapchain->iface, backbuffer_idx,
                                           type, surface);
}

static HRESULT WINAPI dx9mt_device_GetRasterStatus(IDirect3DDevice9 *iface,
                                                    UINT swapchain_idx,
                                                    D3DRASTER_STATUS *status) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (swapchain_idx != 0 || !status) {
    return D3DERR_INVALIDCALL;
  }
  return IDirect3DSwapChain9_GetRasterStatus(&self->swapchain->iface, status);
}

static HRESULT WINAPI dx9mt_device_SetDialogBoxMode(IDirect3DDevice9 *iface,
                                                     WINBOOL enable) {
  (void)iface;
  (void)enable;
  return D3D_OK;
}

static void WINAPI dx9mt_device_SetGammaRamp(IDirect3DDevice9 *iface,
                                              UINT swapchain_idx, DWORD flags,
                                              const D3DGAMMARAMP *ramp) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  (void)swapchain_idx;
  (void)flags;
  if (ramp) {
    self->gamma_ramp = *ramp;
  }
}

static void WINAPI dx9mt_device_GetGammaRamp(IDirect3DDevice9 *iface,
                                              UINT swapchain_idx,
                                              D3DGAMMARAMP *ramp) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  (void)swapchain_idx;
  if (ramp) {
    *ramp = self->gamma_ramp;
  }
}

static HRESULT WINAPI dx9mt_device_CreateTexture(
    IDirect3DDevice9 *iface, UINT width, UINT height, UINT levels, DWORD usage,
    D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9 **texture,
    HANDLE *shared_handle) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  HRESULT hr;
  (void)shared_handle;
  hr = dx9mt_texture_create(self, width, height, levels, usage, format, pool,
                            texture);
  dx9mt_logf("device",
             "CreateTexture %ux%u levels=%u usage=0x%08x fmt=%u pool=%u -> hr=0x%08x",
             width, height, levels, (unsigned)usage, (unsigned)format,
             (unsigned)pool, (unsigned)hr);
  return hr;
}

static HRESULT WINAPI dx9mt_device_CreateVolumeTexture(
    IDirect3DDevice9 *iface, UINT width, UINT height, UINT depth, UINT levels,
    DWORD usage, D3DFORMAT format, D3DPOOL pool,
    IDirect3DVolumeTexture9 **volume_texture, HANDLE *shared_handle) {
  static LONG log_counter = 0;
  (void)iface;
  (void)shared_handle;
  if (volume_texture) {
    *volume_texture = NULL;
  }
  if (dx9mt_should_log_method_sample(&log_counter, 4, 128)) {
    dx9mt_logf("device",
               "CreateVolumeTexture unsupported width=%u height=%u depth=%u levels=%u usage=0x%08x fmt=%u pool=%u -> NOTAVAILABLE",
               width, height, depth, levels, (unsigned)usage, (unsigned)format,
               (unsigned)pool);
  }
  return D3DERR_NOTAVAILABLE;
}

static HRESULT WINAPI dx9mt_device_CreateCubeTexture(
    IDirect3DDevice9 *iface, UINT edge_length, UINT levels, DWORD usage,
    D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9 **cube_texture,
    HANDLE *shared_handle) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  HRESULT hr;
  static LONG log_counter = 0;
  (void)shared_handle;
  if (!cube_texture) {
    return D3DERR_INVALIDCALL;
  }

  hr = dx9mt_cube_texture_create(self, edge_length, levels, usage, format, pool,
                                 cube_texture);

  if (dx9mt_should_log_method_sample(&log_counter, 4, 128)) {
    dx9mt_logf("device",
               "CreateCubeTexture edge=%u levels=%u usage=0x%08x fmt=%u pool=%u -> hr=0x%08x",
               edge_length, levels, (unsigned)usage, (unsigned)format,
               (unsigned)pool, (unsigned)hr);
  }
  return hr;
}

static HRESULT WINAPI dx9mt_device_CreateVertexBuffer(
    IDirect3DDevice9 *iface, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
    IDirect3DVertexBuffer9 **vb, HANDLE *shared_handle) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  HRESULT hr;
  (void)shared_handle;
  hr = dx9mt_vb_create(self, length, usage, fvf, pool, vb);
  dx9mt_logf("device",
             "CreateVertexBuffer len=%u usage=0x%08x fvf=0x%08x pool=%u -> hr=0x%08x",
             length, (unsigned)usage, (unsigned)fvf, (unsigned)pool,
             (unsigned)hr);
  return hr;
}

static HRESULT WINAPI dx9mt_device_CreateIndexBuffer(
    IDirect3DDevice9 *iface, UINT length, DWORD usage, D3DFORMAT format,
    D3DPOOL pool, IDirect3DIndexBuffer9 **ib, HANDLE *shared_handle) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  HRESULT hr;
  (void)shared_handle;
  hr = dx9mt_ib_create(self, length, usage, format, pool, ib);
  dx9mt_logf("device",
             "CreateIndexBuffer len=%u usage=0x%08x fmt=%u pool=%u -> hr=0x%08x",
             length, (unsigned)usage, (unsigned)format, (unsigned)pool,
             (unsigned)hr);
  return hr;
}

static HRESULT WINAPI dx9mt_device_CreateRenderTarget(
    IDirect3DDevice9 *iface, UINT width, UINT height, D3DFORMAT format,
    D3DMULTISAMPLE_TYPE multisample, DWORD quality, WINBOOL lockable,
    IDirect3DSurface9 **surface, HANDLE *shared_handle) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  (void)shared_handle;
  return dx9mt_surface_create(self, width, height, format, D3DPOOL_DEFAULT,
                              D3DUSAGE_RENDERTARGET, multisample, quality,
                              lockable, NULL, surface);
}

static HRESULT WINAPI dx9mt_device_CreateDepthStencilSurface(
    IDirect3DDevice9 *iface, UINT width, UINT height, D3DFORMAT format,
    D3DMULTISAMPLE_TYPE multisample, DWORD quality, WINBOOL discard,
    IDirect3DSurface9 **surface, HANDLE *shared_handle) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  (void)discard;
  (void)shared_handle;
  return dx9mt_surface_create(self, width, height, format, D3DPOOL_DEFAULT,
                              D3DUSAGE_DEPTHSTENCIL, multisample, quality,
                              FALSE, NULL, surface);
}

static HRESULT WINAPI dx9mt_device_CreateOffscreenPlainSurface(
    IDirect3DDevice9 *iface, UINT width, UINT height, D3DFORMAT format,
    D3DPOOL pool, IDirect3DSurface9 **surface, HANDLE *shared_handle) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  (void)shared_handle;
  return dx9mt_surface_create(self, width, height, format, pool, 0,
                              D3DMULTISAMPLE_NONE, 0, TRUE, NULL, surface);
}

static HRESULT WINAPI dx9mt_device_UpdateSurface(
    IDirect3DDevice9 *iface, IDirect3DSurface9 *src_surface,
    const RECT *src_rect, IDirect3DSurface9 *dst_surface,
    const POINT *dst_point) {
  dx9mt_surface *src = dx9mt_surface_from_iface(src_surface);
  dx9mt_surface *dst = dx9mt_surface_from_iface(dst_surface);
  RECT src_r;
  RECT dst_r;
  LONG dst_left = 0;
  LONG dst_top = 0;

  (void)iface;
  if (!src_surface || !dst_surface) {
    return D3DERR_INVALIDCALL;
  }

  if (dst_point) {
    dst_left = dst_point->x;
    dst_top = dst_point->y;
  }

  dx9mt_resolve_rect(&src->desc, src_rect, &src_r);
  if (!dx9mt_rect_valid_for_surface(&src_r, &src->desc)) {
    return D3DERR_INVALIDCALL;
  }

  dst_r.left = dst_left;
  dst_r.top = dst_top;
  dst_r.right = dst_left + (src_r.right - src_r.left);
  dst_r.bottom = dst_top + (src_r.bottom - src_r.top);

  return dx9mt_surface_copy_rect(dst, &dst_r, src, &src_r, FALSE);
}

static HRESULT WINAPI dx9mt_device_UpdateTexture(
    IDirect3DDevice9 *iface, IDirect3DBaseTexture9 *src_texture,
    IDirect3DBaseTexture9 *dst_texture) {
  IDirect3DTexture9 *src_tex2d = NULL;
  IDirect3DTexture9 *dst_tex2d = NULL;
  HRESULT hr = D3D_OK;
  UINT src_levels;
  UINT dst_levels;
  UINT level_count;
  UINT level;

  (void)iface;
  if (!src_texture || !dst_texture) {
    return D3DERR_INVALIDCALL;
  }

  hr = IDirect3DBaseTexture9_QueryInterface(src_texture, &IID_IDirect3DTexture9,
                                            (void **)&src_tex2d);
  if (FAILED(hr)) {
    return D3DERR_INVALIDCALL;
  }

  hr = IDirect3DBaseTexture9_QueryInterface(dst_texture, &IID_IDirect3DTexture9,
                                            (void **)&dst_tex2d);
  if (FAILED(hr)) {
    IDirect3DTexture9_Release(src_tex2d);
    return D3DERR_INVALIDCALL;
  }

  src_levels = IDirect3DTexture9_GetLevelCount(src_tex2d);
  dst_levels = IDirect3DTexture9_GetLevelCount(dst_tex2d);
  level_count = (src_levels < dst_levels) ? src_levels : dst_levels;

  for (level = 0; level < level_count; ++level) {
    IDirect3DSurface9 *src_level = NULL;
    IDirect3DSurface9 *dst_level = NULL;

    hr = IDirect3DTexture9_GetSurfaceLevel(src_tex2d, level, &src_level);
    if (FAILED(hr)) {
      break;
    }

    hr = IDirect3DTexture9_GetSurfaceLevel(dst_tex2d, level, &dst_level);
    if (FAILED(hr)) {
      IDirect3DSurface9_Release(src_level);
      break;
    }

    hr = dx9mt_surface_copy_rect(dx9mt_surface_from_iface(dst_level), NULL,
                                 dx9mt_surface_from_iface(src_level), NULL, FALSE);
    IDirect3DSurface9_Release(dst_level);
    IDirect3DSurface9_Release(src_level);
    if (FAILED(hr)) {
      break;
    }
  }

  IDirect3DTexture9_Release(dst_tex2d);
  IDirect3DTexture9_Release(src_tex2d);
  return hr;
}

static HRESULT WINAPI dx9mt_device_GetRenderTargetData(
    IDirect3DDevice9 *iface, IDirect3DSurface9 *render_target,
    IDirect3DSurface9 *dest_surface) {
  dx9mt_surface *src = dx9mt_surface_from_iface(render_target);
  dx9mt_surface *dst = dx9mt_surface_from_iface(dest_surface);
  (void)iface;
  if (!render_target || !dest_surface) {
    return D3DERR_INVALIDCALL;
  }
  return dx9mt_surface_copy_rect(dst, NULL, src, NULL, FALSE);
}

static HRESULT WINAPI dx9mt_device_GetFrontBufferData(
    IDirect3DDevice9 *iface, UINT swapchain_idx, IDirect3DSurface9 *dest_surface) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (swapchain_idx != 0 || !dest_surface || !self->swapchain ||
      !self->swapchain->backbuffer) {
    return D3DERR_INVALIDCALL;
  }
  return dx9mt_device_GetRenderTargetData(iface, self->swapchain->backbuffer,
                                          dest_surface);
}

static HRESULT WINAPI dx9mt_device_StretchRect(
    IDirect3DDevice9 *iface, IDirect3DSurface9 *src_surface,
    const RECT *src_rect, IDirect3DSurface9 *dst_surface, const RECT *dst_rect,
    D3DTEXTUREFILTERTYPE filter) {
  dx9mt_surface *src = dx9mt_surface_from_iface(src_surface);
  dx9mt_surface *dst = dx9mt_surface_from_iface(dst_surface);
  (void)iface;
  (void)filter;
  if (!src_surface || !dst_surface) {
    return D3DERR_INVALIDCALL;
  }
  return dx9mt_surface_copy_rect(dst, dst_rect, src, src_rect, TRUE);
}

static HRESULT WINAPI dx9mt_device_ColorFill(IDirect3DDevice9 *iface,
                                              IDirect3DSurface9 *surface,
                                              const RECT *rect, D3DCOLOR color) {
  (void)iface;
  if (!surface) {
    return D3DERR_INVALIDCALL;
  }
  return dx9mt_surface_fill_rect(dx9mt_surface_from_iface(surface), rect, color);
}

static HRESULT WINAPI dx9mt_device_SetRenderTarget(IDirect3DDevice9 *iface,
                                                    DWORD index,
                                                    IDirect3DSurface9 *surface) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (index >= DX9MT_MAX_RENDER_TARGETS) {
    return D3DERR_INVALIDCALL;
  }

  if (self->render_targets[index] == surface) {
    return D3D_OK;
  }

  dx9mt_safe_addref((IUnknown *)surface);
  dx9mt_safe_release((IUnknown *)self->render_targets[index]);
  self->render_targets[index] = surface;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetRenderTarget(IDirect3DDevice9 *iface,
                                                    DWORD index,
                                                    IDirect3DSurface9 **surface) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!surface || index >= DX9MT_MAX_RENDER_TARGETS) {
    return D3DERR_INVALIDCALL;
  }

  *surface = self->render_targets[index];
  if (*surface) {
    IDirect3DSurface9_AddRef(*surface);
  }
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetDepthStencilSurface(
    IDirect3DDevice9 *iface, IDirect3DSurface9 *surface) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (self->depth_stencil == surface) {
    return D3D_OK;
  }

  dx9mt_safe_addref((IUnknown *)surface);
  dx9mt_safe_release((IUnknown *)self->depth_stencil);
  self->depth_stencil = surface;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetDepthStencilSurface(
    IDirect3DDevice9 *iface, IDirect3DSurface9 **surface) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!surface) {
    return D3DERR_INVALIDCALL;
  }

  *surface = self->depth_stencil;
  if (*surface) {
    IDirect3DSurface9_AddRef(*surface);
  }
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_BeginScene(IDirect3DDevice9 *iface) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  dx9mt_packet_begin_frame packet;

  if (self->in_scene) {
    return D3DERR_INVALIDCALL;
  }

  self->in_scene = TRUE;

  /*
   * Emit BEGIN_FRAME through the packet stream rather than calling
   * begin_frame() as a side-channel. This ensures the packet stream is
   * self-describing -- every frame boundary is visible in the serialized
   * packet log and will survive a future IPC/shared-memory transport
   * without needing a separate control channel.
   *
   * The backend parser dispatches BEGIN_FRAME packets to the same
   * begin_frame logic, so the behavior is identical.
   */
  memset(&packet, 0, sizeof(packet));
  packet.header.type = DX9MT_PACKET_BEGIN_FRAME;
  packet.header.size = (uint16_t)sizeof(packet);
  packet.header.sequence = dx9mt_runtime_next_packet_sequence();
  packet.frame_id = self->frame_id;
  dx9mt_backend_bridge_submit_packets(&packet.header, (uint32_t)sizeof(packet));

  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_EndScene(IDirect3DDevice9 *iface) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!self->in_scene) {
    return D3DERR_INVALIDCALL;
  }

  self->in_scene = FALSE;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_Clear(IDirect3DDevice9 *iface,
                                          DWORD rect_count,
                                          const D3DRECT *rects, DWORD flags,
                                          D3DCOLOR color, float z,
                                          DWORD stencil) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  dx9mt_packet_clear packet;
  HRESULT hr;
  dx9mt_surface *rt0;
  DWORD i;

  rt0 = dx9mt_surface_from_iface(self->render_targets[0]);
  if ((flags & D3DCLEAR_TARGET) != 0 && rt0) {
    if (rect_count == 0 || !rects) {
      hr = dx9mt_surface_fill_rect(rt0, NULL, color);
      if (FAILED(hr)) {
        return hr;
      }
    } else {
      for (i = 0; i < rect_count; ++i) {
        RECT clear_rect;
        clear_rect.left = rects[i].x1;
        clear_rect.top = rects[i].y1;
        clear_rect.right = rects[i].x2;
        clear_rect.bottom = rects[i].y2;
        hr = dx9mt_surface_fill_rect(rt0, &clear_rect, color);
        if (FAILED(hr)) {
          return hr;
        }
      }
    }
  }

  memset(&packet, 0, sizeof(packet));
  packet.header.type = DX9MT_PACKET_CLEAR;
  packet.header.size = (uint16_t)sizeof(packet);
  packet.header.sequence = dx9mt_runtime_next_packet_sequence();
  packet.frame_id = self->frame_id;
  packet.rect_count = rect_count;
  packet.flags = flags;
  packet.color = color;
  packet.z = z;
  packet.stencil = stencil;

  dx9mt_backend_bridge_submit_packets(&packet.header, (uint32_t)sizeof(packet));
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetTransform(IDirect3DDevice9 *iface,
                                                 D3DTRANSFORMSTATETYPE state,
                                                 const D3DMATRIX *matrix) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!matrix || state >= DX9MT_MAX_TRANSFORM_STATES) {
    return D3DERR_INVALIDCALL;
  }

  self->transforms[state] = *matrix;
  self->transform_set[state] = TRUE;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetTransform(IDirect3DDevice9 *iface,
                                                 D3DTRANSFORMSTATETYPE state,
                                                 D3DMATRIX *matrix) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!matrix || state >= DX9MT_MAX_TRANSFORM_STATES) {
    return D3DERR_INVALIDCALL;
  }

  if (!self->transform_set[state]) {
    return D3DERR_INVALIDCALL;
  }

  *matrix = self->transforms[state];
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetViewport(IDirect3DDevice9 *iface,
                                                const D3DVIEWPORT9 *viewport) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!viewport) {
    return D3DERR_INVALIDCALL;
  }

  self->viewport = *viewport;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetViewport(IDirect3DDevice9 *iface,
                                                D3DVIEWPORT9 *viewport) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!viewport) {
    return D3DERR_INVALIDCALL;
  }

  *viewport = self->viewport;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetClipPlane(IDirect3DDevice9 *iface,
                                                 DWORD index,
                                                 const float *plane) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!plane || index >= 6) {
    return D3DERR_INVALIDCALL;
  }

  memcpy(self->clip_planes[index], plane, sizeof(self->clip_planes[index]));
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetClipPlane(IDirect3DDevice9 *iface,
                                                 DWORD index, float *plane) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!plane || index >= 6) {
    return D3DERR_INVALIDCALL;
  }

  memcpy(plane, self->clip_planes[index], sizeof(self->clip_planes[index]));
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetRenderState(IDirect3DDevice9 *iface,
                                                   D3DRENDERSTATETYPE state,
                                                   DWORD value) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if ((unsigned)state >= DX9MT_MAX_RENDER_STATES) {
    return D3DERR_INVALIDCALL;
  }

  self->render_states[state] = value;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetRenderState(IDirect3DDevice9 *iface,
                                                   D3DRENDERSTATETYPE state,
                                                   DWORD *value) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!value || (unsigned)state >= DX9MT_MAX_RENDER_STATES) {
    return D3DERR_INVALIDCALL;
  }

  *value = self->render_states[state];
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetTexture(IDirect3DDevice9 *iface,
                                               DWORD stage,
                                               IDirect3DBaseTexture9 *texture) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (stage >= DX9MT_MAX_TEXTURE_STAGES) {
    return D3DERR_INVALIDCALL;
  }

  if (self->textures[stage] == texture) {
    return D3D_OK;
  }

  dx9mt_safe_addref((IUnknown *)texture);
  dx9mt_safe_release((IUnknown *)self->textures[stage]);
  self->textures[stage] = texture;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetTexture(IDirect3DDevice9 *iface,
                                               DWORD stage,
                                               IDirect3DBaseTexture9 **texture) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!texture || stage >= DX9MT_MAX_TEXTURE_STAGES) {
    return D3DERR_INVALIDCALL;
  }

  *texture = self->textures[stage];
  if (*texture) {
    IDirect3DBaseTexture9_AddRef(*texture);
  }
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetTextureStageState(
    IDirect3DDevice9 *iface, DWORD stage, D3DTEXTURESTAGESTATETYPE type,
    DWORD value) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (stage >= DX9MT_MAX_TEXTURE_STAGES || type >= DX9MT_MAX_TEXTURE_STAGE_STATES) {
    return D3DERR_INVALIDCALL;
  }

  self->tex_stage_states[stage][type] = value;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetTextureStageState(
    IDirect3DDevice9 *iface, DWORD stage, D3DTEXTURESTAGESTATETYPE type,
    DWORD *value) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!value || stage >= DX9MT_MAX_TEXTURE_STAGES ||
      type >= DX9MT_MAX_TEXTURE_STAGE_STATES) {
    return D3DERR_INVALIDCALL;
  }

  *value = self->tex_stage_states[stage][type];
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetSamplerState(IDirect3DDevice9 *iface,
                                                    DWORD sampler,
                                                    D3DSAMPLERSTATETYPE type,
                                                    DWORD value) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (sampler >= DX9MT_MAX_SAMPLERS || type >= DX9MT_MAX_SAMPLER_STATES) {
    return D3DERR_INVALIDCALL;
  }

  self->sampler_states[sampler][type] = value;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetSamplerState(IDirect3DDevice9 *iface,
                                                    DWORD sampler,
                                                    D3DSAMPLERSTATETYPE type,
                                                    DWORD *value) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!value || sampler >= DX9MT_MAX_SAMPLERS ||
      type >= DX9MT_MAX_SAMPLER_STATES) {
    return D3DERR_INVALIDCALL;
  }

  *value = self->sampler_states[sampler][type];
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetScissorRect(IDirect3DDevice9 *iface,
                                                   const RECT *rect) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!rect) {
    return D3DERR_INVALIDCALL;
  }
  self->scissor_rect = *rect;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetScissorRect(IDirect3DDevice9 *iface,
                                                   RECT *rect) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!rect) {
    return D3DERR_INVALIDCALL;
  }
  *rect = self->scissor_rect;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetSoftwareVertexProcessing(
    IDirect3DDevice9 *iface, WINBOOL software) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  self->software_vp = software;
  return D3D_OK;
}

static WINBOOL WINAPI dx9mt_device_GetSoftwareVertexProcessing(
    IDirect3DDevice9 *iface) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  return self->software_vp;
}

static HRESULT WINAPI dx9mt_device_SetNPatchMode(IDirect3DDevice9 *iface,
                                                  float n_segments) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  self->n_patch_mode = n_segments;
  return D3D_OK;
}

static float WINAPI dx9mt_device_GetNPatchMode(IDirect3DDevice9 *iface) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  return self->n_patch_mode;
}

/*
 * DrawPrimitive: stub -- returns D3D_OK without emitting a packet.
 *
 * FNV exclusively uses DrawIndexedPrimitive, so this no-op doesn't affect
 * the primary target. Returning D3D_OK (rather than E_NOTIMPL) prevents
 * games from falling back to software vertex processing or aborting.
 *
 * When a game that relies on non-indexed draws is targeted, this needs a
 * DRAW_NON_INDEXED packet type and corresponding backend handling. Track
 * via sampled logging to detect if any game actually calls this path.
 */
static HRESULT WINAPI dx9mt_device_DrawPrimitive(IDirect3DDevice9 *iface,
                                                  D3DPRIMITIVETYPE primitive_type,
                                                  UINT start_vertex,
                                                  UINT primitive_count) {
  static LONG log_counter = 0;
  (void)iface;

  if (dx9mt_should_log_method_sample(&log_counter, 4, 256)) {
    dx9mt_logf("device",
               "DrawPrimitive stub primitive_type=%u start_vertex=%u primitive_count=%u",
               (unsigned)primitive_type, start_vertex, primitive_count);
  }
  return D3D_OK;
}

/*
 * Convert a D3D9 FVF bitmask to an array of D3DVERTEXELEMENT9 entries.
 * Returns the number of elements produced (excluding end sentinel).
 * The element order matches the D3D9 FVF vertex data layout:
 *   position → blend weights → normal → psize → diffuse → specular → texcoords
 */
static uint16_t dx9mt_fvf_to_vertex_elements(DWORD fvf,
                                              D3DVERTEXELEMENT9 *elems,
                                              uint16_t max_elems) {
  uint16_t count = 0;
  WORD offset = 0;
  DWORD pos_type;
  DWORD tex_count;
  DWORD i;
  DWORD blend_count;

  if (!elems || max_elems == 0 || fvf == 0) {
    return 0;
  }

  pos_type = fvf & D3DFVF_POSITION_MASK;

  /* Position */
  if (pos_type == D3DFVF_XYZRHW) {
    if (count >= max_elems) return count;
    elems[count].Stream = 0;
    elems[count].Offset = offset;
    elems[count].Type = D3DDECLTYPE_FLOAT4;
    elems[count].Method = 0;
    elems[count].Usage = D3DDECLUSAGE_POSITIONT;
    elems[count].UsageIndex = 0;
    count++;
    offset += 16;
  } else if (pos_type == D3DFVF_XYZ || pos_type == D3DFVF_XYZW) {
    if (count >= max_elems) return count;
    elems[count].Stream = 0;
    elems[count].Offset = offset;
    elems[count].Type = (pos_type == D3DFVF_XYZW) ? D3DDECLTYPE_FLOAT4
                                                   : D3DDECLTYPE_FLOAT3;
    elems[count].Method = 0;
    elems[count].Usage = D3DDECLUSAGE_POSITION;
    elems[count].UsageIndex = 0;
    count++;
    offset += (pos_type == D3DFVF_XYZW) ? 16 : 12;
  } else if (pos_type >= D3DFVF_XYZB1 && pos_type <= D3DFVF_XYZB5) {
    if (count >= max_elems) return count;
    elems[count].Stream = 0;
    elems[count].Offset = offset;
    elems[count].Type = D3DDECLTYPE_FLOAT3;
    elems[count].Method = 0;
    elems[count].Usage = D3DDECLUSAGE_POSITION;
    elems[count].UsageIndex = 0;
    count++;
    offset += 12;
    blend_count = (pos_type - D3DFVF_XYZ) / 2;
    if (blend_count > 0 && count < max_elems) {
      elems[count].Stream = 0;
      elems[count].Offset = offset;
      elems[count].Type = (blend_count == 1) ? D3DDECLTYPE_FLOAT1
                        : (blend_count == 2) ? D3DDECLTYPE_FLOAT2
                        : (blend_count == 3) ? D3DDECLTYPE_FLOAT3
                        :                      D3DDECLTYPE_FLOAT4;
      elems[count].Method = 0;
      elems[count].Usage = D3DDECLUSAGE_BLENDWEIGHT;
      elems[count].UsageIndex = 0;
      count++;
    }
    offset += (WORD)(blend_count * 4);
  }

  /* Normal */
  if (fvf & D3DFVF_NORMAL) {
    if (count < max_elems) {
      elems[count].Stream = 0;
      elems[count].Offset = offset;
      elems[count].Type = D3DDECLTYPE_FLOAT3;
      elems[count].Method = 0;
      elems[count].Usage = D3DDECLUSAGE_NORMAL;
      elems[count].UsageIndex = 0;
      count++;
    }
    offset += 12;
  }

  /* Point size */
  if (fvf & D3DFVF_PSIZE) {
    if (count < max_elems) {
      elems[count].Stream = 0;
      elems[count].Offset = offset;
      elems[count].Type = D3DDECLTYPE_FLOAT1;
      elems[count].Method = 0;
      elems[count].Usage = D3DDECLUSAGE_PSIZE;
      elems[count].UsageIndex = 0;
      count++;
    }
    offset += 4;
  }

  /* Diffuse color */
  if (fvf & D3DFVF_DIFFUSE) {
    if (count < max_elems) {
      elems[count].Stream = 0;
      elems[count].Offset = offset;
      elems[count].Type = D3DDECLTYPE_D3DCOLOR;
      elems[count].Method = 0;
      elems[count].Usage = D3DDECLUSAGE_COLOR;
      elems[count].UsageIndex = 0;
      count++;
    }
    offset += 4;
  }

  /* Specular color */
  if (fvf & D3DFVF_SPECULAR) {
    if (count < max_elems) {
      elems[count].Stream = 0;
      elems[count].Offset = offset;
      elems[count].Type = D3DDECLTYPE_D3DCOLOR;
      elems[count].Method = 0;
      elems[count].Usage = D3DDECLUSAGE_COLOR;
      elems[count].UsageIndex = 1;
      count++;
    }
    offset += 4;
  }

  /* Texture coordinates */
  tex_count = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
  for (i = 0; i < tex_count; ++i) {
    DWORD fmt_bits = (fvf >> (16 + i * 2)) & 0x3;
    BYTE decl_type;
    WORD size;
    switch (fmt_bits) {
    case 0:  decl_type = D3DDECLTYPE_FLOAT2; size = 8;  break;
    case 1:  decl_type = D3DDECLTYPE_FLOAT3; size = 12; break;
    case 2:  decl_type = D3DDECLTYPE_FLOAT4; size = 16; break;
    default: decl_type = D3DDECLTYPE_FLOAT1; size = 4;  break;
    }
    if (count < max_elems) {
      elems[count].Stream = 0;
      elems[count].Offset = offset;
      elems[count].Type = decl_type;
      elems[count].Method = 0;
      elems[count].Usage = D3DDECLUSAGE_TEXCOORD;
      elems[count].UsageIndex = (BYTE)i;
      count++;
    }
    offset += size;
  }

  return count;
}

static void
dx9mt_device_fill_draw_texture_stages(dx9mt_device *self,
                                       dx9mt_packet_draw_indexed *packet) {
  uint32_t stage;

  if (!self || !packet) {
    return;
  }

  /* Stage-0 fixed-function combiner state (TSS path only) */
  packet->tss0_color_op = self->tex_stage_states[0][D3DTSS_COLOROP];
  packet->tss0_color_arg1 = self->tex_stage_states[0][D3DTSS_COLORARG1];
  packet->tss0_color_arg2 = self->tex_stage_states[0][D3DTSS_COLORARG2];
  packet->tss0_alpha_op = self->tex_stage_states[0][D3DTSS_ALPHAOP];
  packet->tss0_alpha_arg1 = self->tex_stage_states[0][D3DTSS_ALPHAARG1];
  packet->tss0_alpha_arg2 = self->tex_stage_states[0][D3DTSS_ALPHAARG2];
  packet->rs_texture_factor = self->render_states[D3DRS_TEXTUREFACTOR];

  /* Global render states */
  packet->rs_alpha_blend_enable = self->render_states[D3DRS_ALPHABLENDENABLE];
  packet->rs_src_blend = self->render_states[D3DRS_SRCBLEND];
  packet->rs_dest_blend = self->render_states[D3DRS_DESTBLEND];
  packet->rs_alpha_test_enable = self->render_states[D3DRS_ALPHATESTENABLE];
  packet->rs_alpha_ref = self->render_states[D3DRS_ALPHAREF];
  packet->rs_alpha_func = self->render_states[D3DRS_ALPHAFUNC];
  packet->rs_zenable = self->render_states[D3DRS_ZENABLE];
  packet->rs_zwriteenable = self->render_states[D3DRS_ZWRITEENABLE];
  packet->rs_zfunc = self->render_states[D3DRS_ZFUNC];
  packet->rs_stencilenable = self->render_states[D3DRS_STENCILENABLE];
  packet->rs_stencilfunc = self->render_states[D3DRS_STENCILFUNC];
  packet->rs_stencilref = self->render_states[D3DRS_STENCILREF];
  packet->rs_stencilmask = self->render_states[D3DRS_STENCILMASK];
  packet->rs_stencilwritemask = self->render_states[D3DRS_STENCILWRITEMASK];
  packet->rs_cull_mode = self->render_states[D3DRS_CULLMODE];
  packet->rs_scissortestenable = self->render_states[D3DRS_SCISSORTESTENABLE];
  packet->rs_blendop = self->render_states[D3DRS_BLENDOP];
  packet->rs_colorwriteenable = self->render_states[D3DRS_COLORWRITEENABLE];
  packet->rs_stencilpass = self->render_states[D3DRS_STENCILPASS];
  packet->rs_stencilfail = self->render_states[D3DRS_STENCILFAIL];
  packet->rs_stencilzfail = self->render_states[D3DRS_STENCILZFAIL];
  packet->rs_fogenable = self->render_states[D3DRS_FOGENABLE];
  packet->rs_fogcolor = self->render_states[D3DRS_FOGCOLOR];
  memcpy(&packet->rs_fogstart, &self->render_states[D3DRS_FOGSTART],
         sizeof(float));
  memcpy(&packet->rs_fogend, &self->render_states[D3DRS_FOGEND], sizeof(float));
  memcpy(&packet->rs_fogdensity, &self->render_states[D3DRS_FOGDENSITY],
         sizeof(float));
  packet->rs_fogtablemode = self->render_states[D3DRS_FOGTABLEMODE];

  /* Per-stage texture and sampler data */
  for (stage = 0; stage < DX9MT_MAX_PS_SAMPLERS; ++stage) {
    IDirect3DBaseTexture9 *base_texture;
    D3DRESOURCETYPE type;
    dx9mt_texture *texture;
    uint32_t level;
    dx9mt_surface *surface;
    uint32_t upload_size;
    WINBOOL should_upload;

    packet->sampler_min_filter[stage] = self->sampler_states[stage][D3DSAMP_MINFILTER];
    packet->sampler_mag_filter[stage] = self->sampler_states[stage][D3DSAMP_MAGFILTER];
    packet->sampler_mip_filter[stage] = self->sampler_states[stage][D3DSAMP_MIPFILTER];
    packet->sampler_address_u[stage] = self->sampler_states[stage][D3DSAMP_ADDRESSU];
    packet->sampler_address_v[stage] = self->sampler_states[stage][D3DSAMP_ADDRESSV];
    packet->sampler_address_w[stage] = self->sampler_states[stage][D3DSAMP_ADDRESSW];

    base_texture = self->textures[stage];
    if (!base_texture) {
      continue;
    }

    type = IDirect3DBaseTexture9_GetType(base_texture);
    if (type != D3DRTYPE_TEXTURE) {
      continue;
    }

    texture = dx9mt_texture_from_iface((IDirect3DTexture9 *)base_texture);
    if (!texture || texture->levels == 0 || !texture->surfaces) {
      continue;
    }

    level = texture->lod;
    if (level >= texture->levels) {
      level = 0;
    }
    if (!texture->surfaces[level]) {
      continue;
    }
    surface = dx9mt_surface_from_iface(texture->surfaces[level]);

    packet->tex_id[stage] = texture->object_id;
    packet->tex_generation[stage] = texture->generation;
    packet->tex_format[stage] = (uint32_t)texture->format;
    packet->tex_width[stage] = texture->width >> level;
    packet->tex_height[stage] = texture->height >> level;
    if (packet->tex_width[stage] == 0) {
      packet->tex_width[stage] = 1;
    }
    if (packet->tex_height[stage] == 0) {
      packet->tex_height[stage] = 1;
    }
    packet->tex_pitch[stage] = surface->pitch;

    if (!surface->sysmem) {
      continue;
    }

    upload_size = dx9mt_surface_upload_size(surface);
    if (upload_size == 0) {
      continue;
    }

    should_upload = FALSE;
    if (texture->last_upload_generation != texture->generation) {
      should_upload = TRUE;
    } else if (texture->last_upload_frame_id != self->frame_id) {
      if (((self->frame_id + texture->object_id) %
           DX9MT_TEXTURE_UPLOAD_REFRESH_INTERVAL) == 0u) {
        should_upload = TRUE;
      }
    }
    if (!should_upload) {
      continue;
    }

    packet->tex_data[stage] =
        dx9mt_frontend_upload_copy(self->frame_id, surface->sysmem, upload_size);
    if (packet->tex_data[stage].size > 0) {
      texture->last_upload_generation = texture->generation;
      texture->last_upload_frame_id = self->frame_id;
    }
  }
}

static HRESULT WINAPI dx9mt_device_DrawIndexedPrimitive(
    IDirect3DDevice9 *iface, D3DPRIMITIVETYPE primitive_type,
    INT base_vertex_index, UINT min_vertex_index, UINT num_vertices,
    UINT start_index, UINT prim_count) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  dx9mt_packet_draw_indexed packet;

  memset(&packet, 0, sizeof(packet));
  packet.header.type = DX9MT_PACKET_DRAW_INDEXED;
  packet.header.size = (uint16_t)sizeof(packet);
  packet.header.sequence = dx9mt_runtime_next_packet_sequence();
  packet.primitive_type = primitive_type;
  packet.base_vertex = base_vertex_index;
  packet.min_vertex_index = min_vertex_index;
  packet.num_vertices = num_vertices;
  packet.start_index = start_index;
  packet.primitive_count = prim_count;
  packet.render_target_id =
      dx9mt_surface_object_id_from_iface(self->render_targets[0]);
  packet.depth_stencil_id =
      dx9mt_surface_object_id_from_iface(self->depth_stencil);
  if (self->render_targets[0]) {
    dx9mt_surface *rt0 = dx9mt_surface_from_iface(self->render_targets[0]);
    packet.render_target_texture_id = dx9mt_surface_container_texture_id(rt0);
    packet.render_target_width = rt0->desc.Width;
    packet.render_target_height = rt0->desc.Height;
    packet.render_target_format = (uint32_t)rt0->desc.Format;
  }
  packet.vertex_buffer_id =
      dx9mt_vb_object_id_from_iface(self->streams[0]);
  packet.index_buffer_id = dx9mt_ib_object_id_from_iface(self->indices);
  packet.vertex_decl_id =
      dx9mt_vdecl_object_id_from_iface(self->vertex_decl);
  packet.vertex_shader_id =
      dx9mt_vshader_object_id_from_iface(self->vertex_shader);
  packet.pixel_shader_id =
      dx9mt_pshader_object_id_from_iface(self->pixel_shader);
  packet.fvf = self->fvf;
  packet.stream0_offset = self->stream_offsets[0];
  packet.stream0_stride = self->stream_strides[0];
  packet.viewport_hash = dx9mt_hash_viewport(&self->viewport);
  packet.scissor_hash = dx9mt_hash_rect(&self->scissor_rect);
  packet.texture_stage_hash = dx9mt_hash_texture_stage_state(self);
  packet.sampler_state_hash = dx9mt_hash_sampler_state(self);
  packet.stream_binding_hash = dx9mt_hash_stream_bindings(self);
  if (self->vs_const_dirty || self->vs_const_last_ref.size == 0) {
    self->vs_const_last_ref = dx9mt_frontend_upload_copy(
        self->frame_id, &self->vs_const_f[0][0], DX9MT_DRAW_SHADER_CONSTANT_BYTES);
    self->vs_const_dirty = FALSE;
  }
  packet.constants_vs = self->vs_const_last_ref;
  if (self->ps_const_dirty || self->ps_const_last_ref.size == 0) {
    self->ps_const_last_ref = dx9mt_frontend_upload_copy(
        self->frame_id, &self->ps_const_f[0][0], DX9MT_DRAW_SHADER_CONSTANT_BYTES);
    self->ps_const_dirty = FALSE;
  }
  packet.constants_ps = self->ps_const_last_ref;

  /* RB3 Phase 3: shader bytecode for translation */
  {
    dx9mt_vertex_shader *vs = self->vertex_shader
        ? dx9mt_vshader_from_iface(self->vertex_shader) : NULL;
    dx9mt_pixel_shader *ps = self->pixel_shader
        ? dx9mt_pshader_from_iface(self->pixel_shader) : NULL;
    if (vs && vs->byte_code && vs->dword_count > 0) {
      packet.vs_bytecode = dx9mt_frontend_upload_copy(
          self->frame_id, vs->byte_code, vs->dword_count * sizeof(DWORD));
      packet.vs_bytecode_dwords = vs->dword_count;
    }
    if (ps && ps->byte_code && ps->dword_count > 0) {
      packet.ps_bytecode = dx9mt_frontend_upload_copy(
          self->frame_id, ps->byte_code, ps->dword_count * sizeof(DWORD));
      packet.ps_bytecode_dwords = ps->dword_count;
    }
  }

  /* RB3: actual viewport/scissor values */
  packet.viewport_x = self->viewport.X;
  packet.viewport_y = self->viewport.Y;
  packet.viewport_width = self->viewport.Width;
  packet.viewport_height = self->viewport.Height;
  packet.viewport_min_z = self->viewport.MinZ;
  packet.viewport_max_z = self->viewport.MaxZ;
  packet.scissor_left = (int32_t)self->scissor_rect.left;
  packet.scissor_top = (int32_t)self->scissor_rect.top;
  packet.scissor_right = (int32_t)self->scissor_rect.right;
  packet.scissor_bottom = (int32_t)self->scissor_rect.bottom;

  /* RB3: geometry data -- VB/IB bytes and vertex declaration */
  {
    dx9mt_vertex_buffer *vb =
        self->streams[0] ? dx9mt_vb_from_iface(self->streams[0]) : NULL;
    dx9mt_index_buffer *ib =
        self->indices ? dx9mt_ib_from_iface(self->indices) : NULL;
    dx9mt_vertex_decl *decl =
        self->vertex_decl ? dx9mt_vdecl_from_iface(self->vertex_decl) : NULL;

    if (vb && vb->data && vb->desc.Size > 0) {
      packet.vertex_data = dx9mt_frontend_upload_copy(
          self->frame_id, vb->data, vb->desc.Size);
      packet.vertex_data_size = vb->desc.Size;
    }
    if (ib && ib->data && ib->desc.Size > 0) {
      packet.index_data = dx9mt_frontend_upload_copy(
          self->frame_id, ib->data, ib->desc.Size);
      packet.index_data_size = ib->desc.Size;
      packet.index_format = (uint32_t)ib->desc.Format;
    }
    if (decl && decl->elements && decl->count > 0) {
      packet.vertex_decl_data = dx9mt_frontend_upload_copy(
          self->frame_id, decl->elements,
          decl->count * (uint32_t)sizeof(D3DVERTEXELEMENT9));
      packet.vertex_decl_count = (uint16_t)decl->count;
    } else if (self->fvf != 0) {
      /* No vertex declaration -- synthesize from FVF code. */
      D3DVERTEXELEMENT9 fvf_elems[16];
      uint16_t fvf_count =
          dx9mt_fvf_to_vertex_elements(self->fvf, fvf_elems, 16);
      if (fvf_count > 0) {
        packet.vertex_decl_data = dx9mt_frontend_upload_copy(
            self->frame_id, fvf_elems,
            fvf_count * (uint32_t)sizeof(D3DVERTEXELEMENT9));
        packet.vertex_decl_count = fvf_count;
      }
    }
  }

  dx9mt_device_fill_draw_texture_stages(self, &packet);

  packet.state_block_hash = dx9mt_hash_draw_state(&packet);

  dx9mt_backend_bridge_submit_packets(&packet.header, (uint32_t)sizeof(packet));
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_CreateVertexDeclaration(
    IDirect3DDevice9 *iface, const D3DVERTEXELEMENT9 *elements,
    IDirect3DVertexDeclaration9 **declaration) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  return dx9mt_vdecl_create(self, elements, declaration);
}

static HRESULT WINAPI dx9mt_device_SetVertexDeclaration(
    IDirect3DDevice9 *iface, IDirect3DVertexDeclaration9 *decl) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (self->vertex_decl == decl) {
    return D3D_OK;
  }

  dx9mt_safe_addref((IUnknown *)decl);
  dx9mt_safe_release((IUnknown *)self->vertex_decl);
  self->vertex_decl = decl;
  /* In real D3D9, SetVertexDeclaration replaces the active FVF. */
  self->fvf = 0;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetVertexDeclaration(
    IDirect3DDevice9 *iface, IDirect3DVertexDeclaration9 **decl) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!decl) {
    return D3DERR_INVALIDCALL;
  }

  *decl = self->vertex_decl;
  if (*decl) {
    IDirect3DVertexDeclaration9_AddRef(*decl);
  }
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetFVF(IDirect3DDevice9 *iface, DWORD fvf) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  self->fvf = fvf;
  /* In real D3D9, SetFVF replaces the active vertex declaration. */
  dx9mt_safe_release((IUnknown *)self->vertex_decl);
  self->vertex_decl = NULL;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetFVF(IDirect3DDevice9 *iface, DWORD *fvf) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!fvf) {
    return D3DERR_INVALIDCALL;
  }

  *fvf = self->fvf;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_CreateVertexShader(
    IDirect3DDevice9 *iface, const DWORD *byte_code,
    IDirect3DVertexShader9 **shader) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  HRESULT hr = dx9mt_vshader_create(self, byte_code, shader);
  dx9mt_logf("device", "CreateVertexShader bytecode=%p -> hr=0x%08x shader=%p",
             (const void *)byte_code, (unsigned)hr,
             (shader && *shader) ? (void *)*shader : NULL);
  return hr;
}

static HRESULT WINAPI dx9mt_device_SetVertexShader(IDirect3DDevice9 *iface,
                                                    IDirect3DVertexShader9 *shader) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (self->vertex_shader == shader) {
    return D3D_OK;
  }

  dx9mt_safe_addref((IUnknown *)shader);
  dx9mt_safe_release((IUnknown *)self->vertex_shader);
  self->vertex_shader = shader;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetVertexShader(
    IDirect3DDevice9 *iface, IDirect3DVertexShader9 **shader) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!shader) {
    return D3DERR_INVALIDCALL;
  }

  *shader = self->vertex_shader;
  if (*shader) {
    IDirect3DVertexShader9_AddRef(*shader);
  }
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetVertexShaderConstantF(
    IDirect3DDevice9 *iface, UINT reg_idx, const float *data, UINT count) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!data || reg_idx + count > DX9MT_MAX_SHADER_FLOAT_CONSTANTS) {
    return D3DERR_INVALIDCALL;
  }

  memcpy(&self->vs_const_f[reg_idx][0], data, count * sizeof(self->vs_const_f[0]));
  self->vs_const_dirty = TRUE;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetVertexShaderConstantF(
    IDirect3DDevice9 *iface, UINT reg_idx, float *data, UINT count) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!data || reg_idx + count > DX9MT_MAX_SHADER_FLOAT_CONSTANTS) {
    return D3DERR_INVALIDCALL;
  }

  memcpy(data, &self->vs_const_f[reg_idx][0], count * sizeof(self->vs_const_f[0]));
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetVertexShaderConstantI(
    IDirect3DDevice9 *iface, UINT reg_idx, const int *data, UINT count) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!data || reg_idx + count > DX9MT_MAX_SHADER_INT_CONSTANTS) {
    return D3DERR_INVALIDCALL;
  }

  memcpy(&self->vs_const_i[reg_idx][0], data, count * sizeof(self->vs_const_i[0]));
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetVertexShaderConstantI(
    IDirect3DDevice9 *iface, UINT reg_idx, int *data, UINT count) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!data || reg_idx + count > DX9MT_MAX_SHADER_INT_CONSTANTS) {
    return D3DERR_INVALIDCALL;
  }

  memcpy(data, &self->vs_const_i[reg_idx][0], count * sizeof(self->vs_const_i[0]));
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetVertexShaderConstantB(
    IDirect3DDevice9 *iface, UINT reg_idx, const WINBOOL *data, UINT count) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!data || reg_idx + count > DX9MT_MAX_SHADER_BOOL_CONSTANTS) {
    return D3DERR_INVALIDCALL;
  }

  memcpy(&self->vs_const_b[reg_idx], data, count * sizeof(WINBOOL));
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetVertexShaderConstantB(
    IDirect3DDevice9 *iface, UINT reg_idx, WINBOOL *data, UINT count) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!data || reg_idx + count > DX9MT_MAX_SHADER_BOOL_CONSTANTS) {
    return D3DERR_INVALIDCALL;
  }

  memcpy(data, &self->vs_const_b[reg_idx], count * sizeof(WINBOOL));
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetStreamSource(
    IDirect3DDevice9 *iface, UINT stream_number,
    IDirect3DVertexBuffer9 *stream_data, UINT offset_in_bytes, UINT stride) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (stream_number >= DX9MT_MAX_STREAMS) {
    return D3DERR_INVALIDCALL;
  }

  if (self->streams[stream_number] != stream_data) {
    dx9mt_safe_addref((IUnknown *)stream_data);
    dx9mt_safe_release((IUnknown *)self->streams[stream_number]);
    self->streams[stream_number] = stream_data;
  }

  self->stream_offsets[stream_number] = offset_in_bytes;
  self->stream_strides[stream_number] = stride;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetStreamSource(
    IDirect3DDevice9 *iface, UINT stream_number,
    IDirect3DVertexBuffer9 **stream_data, UINT *offset_in_bytes, UINT *stride) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (stream_number >= DX9MT_MAX_STREAMS || !stream_data || !offset_in_bytes ||
      !stride) {
    return D3DERR_INVALIDCALL;
  }

  *stream_data = self->streams[stream_number];
  if (*stream_data) {
    IDirect3DVertexBuffer9_AddRef(*stream_data);
  }
  *offset_in_bytes = self->stream_offsets[stream_number];
  *stride = self->stream_strides[stream_number];
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetStreamSourceFreq(IDirect3DDevice9 *iface,
                                                        UINT stream_number,
                                                        UINT divider) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (stream_number >= DX9MT_MAX_STREAMS) {
    return D3DERR_INVALIDCALL;
  }

  self->stream_freq[stream_number] = divider;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetStreamSourceFreq(IDirect3DDevice9 *iface,
                                                        UINT stream_number,
                                                        UINT *divider) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (stream_number >= DX9MT_MAX_STREAMS || !divider) {
    return D3DERR_INVALIDCALL;
  }

  *divider = self->stream_freq[stream_number];
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetIndices(IDirect3DDevice9 *iface,
                                               IDirect3DIndexBuffer9 *index_data) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (self->indices == index_data) {
    return D3D_OK;
  }

  dx9mt_safe_addref((IUnknown *)index_data);
  dx9mt_safe_release((IUnknown *)self->indices);
  self->indices = index_data;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetIndices(IDirect3DDevice9 *iface,
                                               IDirect3DIndexBuffer9 **index_data) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!index_data) {
    return D3DERR_INVALIDCALL;
  }

  *index_data = self->indices;
  if (*index_data) {
    IDirect3DIndexBuffer9_AddRef(*index_data);
  }
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_CreatePixelShader(
    IDirect3DDevice9 *iface, const DWORD *byte_code,
    IDirect3DPixelShader9 **shader) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  HRESULT hr = dx9mt_pshader_create(self, byte_code, shader);
  dx9mt_logf("device", "CreatePixelShader bytecode=%p -> hr=0x%08x shader=%p",
             (const void *)byte_code, (unsigned)hr,
             (shader && *shader) ? (void *)*shader : NULL);
  return hr;
}

static HRESULT WINAPI dx9mt_device_SetPixelShader(IDirect3DDevice9 *iface,
                                                   IDirect3DPixelShader9 *shader) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (self->pixel_shader == shader) {
    return D3D_OK;
  }

  dx9mt_safe_addref((IUnknown *)shader);
  dx9mt_safe_release((IUnknown *)self->pixel_shader);
  self->pixel_shader = shader;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetPixelShader(IDirect3DDevice9 *iface,
                                                   IDirect3DPixelShader9 **shader) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!shader) {
    return D3DERR_INVALIDCALL;
  }

  *shader = self->pixel_shader;
  if (*shader) {
    IDirect3DPixelShader9_AddRef(*shader);
  }
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetPixelShaderConstantF(
    IDirect3DDevice9 *iface, UINT reg_idx, const float *data, UINT count) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!data || reg_idx + count > DX9MT_MAX_SHADER_FLOAT_CONSTANTS) {
    return D3DERR_INVALIDCALL;
  }

  memcpy(&self->ps_const_f[reg_idx][0], data, count * sizeof(self->ps_const_f[0]));
  self->ps_const_dirty = TRUE;
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetPixelShaderConstantF(
    IDirect3DDevice9 *iface, UINT reg_idx, float *data, UINT count) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!data || reg_idx + count > DX9MT_MAX_SHADER_FLOAT_CONSTANTS) {
    return D3DERR_INVALIDCALL;
  }

  memcpy(data, &self->ps_const_f[reg_idx][0], count * sizeof(self->ps_const_f[0]));
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetPixelShaderConstantI(
    IDirect3DDevice9 *iface, UINT reg_idx, const int *data, UINT count) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!data || reg_idx + count > DX9MT_MAX_SHADER_INT_CONSTANTS) {
    return D3DERR_INVALIDCALL;
  }

  memcpy(&self->ps_const_i[reg_idx][0], data, count * sizeof(self->ps_const_i[0]));
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetPixelShaderConstantI(
    IDirect3DDevice9 *iface, UINT reg_idx, int *data, UINT count) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!data || reg_idx + count > DX9MT_MAX_SHADER_INT_CONSTANTS) {
    return D3DERR_INVALIDCALL;
  }

  memcpy(data, &self->ps_const_i[reg_idx][0], count * sizeof(self->ps_const_i[0]));
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_SetPixelShaderConstantB(
    IDirect3DDevice9 *iface, UINT reg_idx, const WINBOOL *data, UINT count) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!data || reg_idx + count > DX9MT_MAX_SHADER_BOOL_CONSTANTS) {
    return D3DERR_INVALIDCALL;
  }

  memcpy(&self->ps_const_b[reg_idx], data, count * sizeof(WINBOOL));
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_GetPixelShaderConstantB(
    IDirect3DDevice9 *iface, UINT reg_idx, WINBOOL *data, UINT count) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  if (!data || reg_idx + count > DX9MT_MAX_SHADER_BOOL_CONSTANTS) {
    return D3DERR_INVALIDCALL;
  }

  memcpy(data, &self->ps_const_b[reg_idx], count * sizeof(WINBOOL));
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_device_CreateQuery(IDirect3DDevice9 *iface,
                                                D3DQUERYTYPE type,
                                                IDirect3DQuery9 **query) {
  dx9mt_device *self = dx9mt_device_from_iface(iface);
  dx9mt_query *object;

  if (!query) {
    return D3DERR_INVALIDCALL;
  }

  *query = NULL;

  object = (dx9mt_query *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                    sizeof(dx9mt_query));
  if (!object) {
    return E_OUTOFMEMORY;
  }

  object->iface.lpVtbl = (IDirect3DQuery9Vtbl *)&g_dx9mt_query_vtbl;
  object->refcount = 1;
  object->object_id = dx9mt_alloc_object_id(DX9MT_OBJECT_KIND_QUERY);
  object->device = self;
  object->type = type;
  object->data_size = dx9mt_query_data_size(type);
  object->issued = FALSE;
  object->issue_flags = 0;

  IDirect3DDevice9_AddRef(iface);
  *query = &object->iface;

  dx9mt_logf("device", "CreateQuery type=%u -> ok", (unsigned)type);
  return D3D_OK;
}

HRESULT dx9mt_device_create(IDirect3D9 *parent, UINT adapter,
                            D3DDEVTYPE device_type, HWND focus_window,
                            DWORD behavior_flags,
                            D3DPRESENT_PARAMETERS *presentation_parameters,
                            IDirect3DDevice9 **returned_device_interface) {
  dx9mt_device *device;
  HRESULT hr;

  if (!presentation_parameters || !returned_device_interface) {
    return D3DERR_INVALIDCALL;
  }

  *returned_device_interface = NULL;

  dx9mt_device_init_vtbl();

  device = (dx9mt_device *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                     sizeof(dx9mt_device));
  if (!device) {
    return E_OUTOFMEMORY;
  }

  device->iface.lpVtbl = &g_dx9mt_device_vtbl;
  device->refcount = 1;
  device->parent = parent;
  device->adapter = adapter;
  device->device_type = device_type;
  device->focus_window = focus_window;
  device->behavior_flags = behavior_flags;
  device->software_vp = (behavior_flags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) != 0;
  device->frame_id = 1;
  dx9mt_device_init_default_states(device);

  if (parent) {
    IDirect3D9_AddRef(parent);
  }

  device->creation.AdapterOrdinal = adapter;
  device->creation.DeviceType = device_type;
  device->creation.hFocusWindow = focus_window;
  device->creation.BehaviorFlags = behavior_flags;

  if (presentation_parameters->BackBufferCount == 0) {
    presentation_parameters->BackBufferCount = 1;
  }
  if (presentation_parameters->SwapEffect == 0) {
    presentation_parameters->SwapEffect = D3DSWAPEFFECT_DISCARD;
  }

  hr = dx9mt_device_reset_internal(device, presentation_parameters);
  if (FAILED(hr)) {
    dx9mt_safe_release((IUnknown *)parent);
    HeapFree(GetProcessHeap(), 0, device);
    return hr;
  }

  memset(&device->gamma_ramp, 0, sizeof(device->gamma_ramp));

  *returned_device_interface = &device->iface;
  dx9mt_logf("device", "CreateDevice success adapter=%u behavior=0x%08x backbuffer=%ux%u",
             adapter, behavior_flags, device->viewport.Width, device->viewport.Height);
  return D3D_OK;
}
