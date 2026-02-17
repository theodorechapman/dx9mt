/*
 * dx9mt Metal Viewer -- standalone native macOS process.
 *
 * Reads frame descriptors + geometry data from a shared memory-mapped
 * file written by the PE DLL (running under Wine) and renders them
 * using Metal. Bridges the PE32/native gap without Wine unix lib integration.
 */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dx9mt/metal_ipc.h"
#include "d3d9_shader_parse.h"
#include "d3d9_shader_emit_msl.h"

/* D3D9 constants we need to interpret vertex declarations and draw params */
enum {
  D3DDECLTYPE_FLOAT1 = 0,
  D3DDECLTYPE_FLOAT2 = 1,
  D3DDECLTYPE_FLOAT3 = 2,
  D3DDECLTYPE_FLOAT4 = 3,
  D3DDECLTYPE_D3DCOLOR = 4,
  D3DDECLTYPE_UBYTE4 = 5,
  D3DDECLTYPE_SHORT2 = 6,
  D3DDECLTYPE_SHORT4 = 7,
  D3DDECLTYPE_UBYTE4N = 8,
  D3DDECLTYPE_SHORT2N = 9,
  D3DDECLTYPE_SHORT4N = 10,
  D3DDECLTYPE_USHORT2N = 11,
  D3DDECLTYPE_USHORT4N = 12,
  D3DDECLTYPE_FLOAT16_2 = 15,
  D3DDECLTYPE_FLOAT16_4 = 16,
};

enum {
  D3DDECLUSAGE_POSITION = 0,
  D3DDECLUSAGE_NORMAL = 3,
  D3DDECLUSAGE_TEXCOORD = 5,
  D3DDECLUSAGE_POSITIONT = 9,
  D3DDECLUSAGE_COLOR = 10,
};

enum {
  D3DFVF_POSITION_MASK = 0x400E,
  D3DFVF_XYZ = 0x0002,
  D3DFVF_XYZRHW = 0x0004,
  D3DFVF_XYZB1 = 0x0006,
  D3DFVF_XYZB5 = 0x000E,
  D3DFVF_XYZW = 0x4002,
  D3DFVF_NORMAL = 0x0010,
  D3DFVF_PSIZE = 0x0020,
  D3DFVF_DIFFUSE = 0x0040,
  D3DFVF_SPECULAR = 0x0080,
  D3DFVF_TEXCOUNT_MASK = 0x0F00,
  D3DFVF_TEXCOUNT_SHIFT = 8,
};

enum {
  D3DPT_TRIANGLELIST = 4,
  D3DPT_TRIANGLESTRIP = 5,
  D3DPT_TRIANGLEFAN = 6,
};

enum {
  D3DFMT_INDEX16 = 101,
  D3DFMT_INDEX32 = 102,
};

enum {
  D3DFMT_A8R8G8B8 = 21,
  D3DFMT_X8R8G8B8 = 22,
  D3DFMT_A8 = 28,
  D3DFMT_DXT1 = ('D' | ('X' << 8) | ('T' << 16) | ('1' << 24)),
  D3DFMT_DXT3 = ('D' | ('X' << 8) | ('T' << 16) | ('3' << 24)),
  D3DFMT_DXT5 = ('D' | ('X' << 8) | ('T' << 16) | ('5' << 24)),
};

enum {
  D3DTEXF_NONE = 0,
  D3DTEXF_POINT = 1,
  D3DTEXF_LINEAR = 2,
  D3DTEXF_ANISOTROPIC = 3,
};

enum {
  D3DTADDRESS_WRAP = 1,
  D3DTADDRESS_MIRROR = 2,
  D3DTADDRESS_CLAMP = 3,
  D3DTADDRESS_BORDER = 4,
  D3DTADDRESS_MIRRORONCE = 5,
};

enum {
  D3DBLEND_ZERO = 1,
  D3DBLEND_ONE = 2,
  D3DBLEND_SRCCOLOR = 3,
  D3DBLEND_INVSRCCOLOR = 4,
  D3DBLEND_SRCALPHA = 5,
  D3DBLEND_INVSRCALPHA = 6,
  D3DBLEND_DESTALPHA = 7,
  D3DBLEND_INVDESTALPHA = 8,
  D3DBLEND_DESTCOLOR = 9,
  D3DBLEND_INVDESTCOLOR = 10,
  D3DBLEND_SRCALPHASAT = 11,
  D3DBLEND_BOTHSRCALPHA = 12,
  D3DBLEND_BOTHINVSRCALPHA = 13,
};

enum {
  D3DCMP_NEVER = 1,
  D3DCMP_LESS = 2,
  D3DCMP_EQUAL = 3,
  D3DCMP_LESSEQUAL = 4,
  D3DCMP_GREATER = 5,
  D3DCMP_NOTEQUAL = 6,
  D3DCMP_GREATEREQUAL = 7,
  D3DCMP_ALWAYS = 8,
};

enum {
  D3DTOP_DISABLE = 1,
  D3DTOP_SELECTARG1 = 2,
  D3DTOP_SELECTARG2 = 3,
  D3DTOP_MODULATE = 4,
  D3DTOP_MODULATE2X = 5,
  D3DTOP_MODULATE4X = 6,
  D3DTOP_ADD = 7,
  D3DTOP_ADDSIGNED = 8,
  D3DTOP_ADDSIGNED2X = 9,
  D3DTOP_SUBTRACT = 10,
  D3DTOP_ADDSMOOTH = 11,
  D3DTOP_BLENDDIFFUSEALPHA = 12,
  D3DTOP_BLENDTEXTUREALPHA = 13,
  D3DTOP_BLENDFACTORALPHA = 14,
  D3DTOP_BLENDTEXTUREALPHAPM = 15,
  D3DTOP_BLENDCURRENTALPHA = 16,
  D3DTOP_PREMODULATE = 17,
};

enum {
  D3DTA_DIFFUSE = 0x00000000,
  D3DTA_CURRENT = 0x00000001,
  D3DTA_TEXTURE = 0x00000002,
  D3DTA_TFACTOR = 0x00000003,
  D3DTA_SPECULAR = 0x00000004,
  D3DTA_TEMP = 0x00000005,
  D3DTA_CONSTANT = 0x00000006,
  D3DTA_COMPLEMENT = 0x00000010,
  D3DTA_ALPHAREPLICATE = 0x00000020,
};

/* D3DVERTEXELEMENT9 layout -- must match Win32 struct (8 bytes packed) */
#pragma pack(push, 1)
typedef struct dx9mt_d3d_vertex_element {
  uint16_t stream;
  uint16_t offset;
  uint8_t type;
  uint8_t method;
  uint8_t usage;
  uint8_t usage_index;
} dx9mt_d3d_vertex_element;
#pragma pack(pop)

static volatile int s_dump_next_frame = 0;
static volatile int s_dump_continuous = 0;
static uint32_t s_dump_seq = 0;

static id<MTLDevice> s_device;
static id<MTLCommandQueue> s_queue;
static id<MTLRenderPipelineState> s_overlay_pso;
static id<MTLRenderPipelineState> s_geometry_pso;
static id<MTLRenderPipelineState> s_geometry_textured_pso;
static id<MTLLibrary> s_library;
static uint32_t s_geometry_pso_stride;
static uint32_t s_geometry_textured_pso_stride;
static uint64_t s_geometry_pso_key;
static uint64_t s_geometry_textured_pso_key;
static NSMutableDictionary *s_texture_cache;
static NSMutableDictionary *s_texture_generation;
static NSMutableDictionary *s_sampler_cache;
static NSMutableDictionary *s_render_target_cache;
static NSMutableDictionary *s_render_target_desc;
static NSMutableDictionary *s_texture_rt_overrides;
static CAMetalLayer *s_metal_layer;
static NSWindow *s_window;
static uint32_t s_width = 1280;
static uint32_t s_height = 720;
static char s_output_dir[PATH_MAX];
static int s_output_dir_ready = 0;
static int s_output_dir_warned = 0;

/* RB3 Phase 3: shader translation caches */
static NSMutableDictionary *s_vs_func_cache;  /* bytecode_hash -> id<MTLFunction> or NSNull */
static NSMutableDictionary *s_ps_func_cache;
static NSMutableDictionary *s_translated_pso_cache;  /* combined_key -> id<MTLRenderPipelineState> */

/* RB4: depth/stencil */
static NSMutableDictionary *s_depth_texture_cache;  /* rt_id -> id<MTLTexture> (Depth32Float) */
static id<MTLTexture> s_drawable_depth_texture;
static uint32_t s_drawable_depth_w;
static uint32_t s_drawable_depth_h;
static NSMutableDictionary *s_depth_stencil_cache;  /* key -> id<MTLDepthStencilState> */
static id<MTLDepthStencilState> s_no_depth_state;   /* always-pass, no write (overlay) */

static const char *dx9mt_output_dir(void) {
  const char *env;

  if (s_output_dir_ready) {
    return s_output_dir;
  }

  env = getenv("DX9MT_OUTPUT_DIR");
  if (env && env[0] != '\0') {
    snprintf(s_output_dir, sizeof(s_output_dir), "%s", env);
  } else if (!getcwd(s_output_dir, sizeof(s_output_dir))) {
    snprintf(s_output_dir, sizeof(s_output_dir), ".");
  }

  if (!env || env[0] == '\0') {
    size_t len = strlen(s_output_dir);
    const char *suffix = "/dx9mt-output";
    if (len + strlen(suffix) + 1 < sizeof(s_output_dir)) {
      snprintf(s_output_dir + len, sizeof(s_output_dir) - len, "%s", suffix);
    }
  }

  s_output_dir_ready = 1;
  return s_output_dir;
}

static void ensure_output_dir(void) {
  static int attempted = 0;
  NSString *dir_path;
  NSError *err = nil;
  BOOL ok;

  if (attempted) {
    return;
  }
  attempted = 1;

  dir_path = [NSString stringWithUTF8String:dx9mt_output_dir()];
  if (!dir_path) {
    return;
  }

  ok = [[NSFileManager defaultManager] createDirectoryAtPath:dir_path
                                 withIntermediateDirectories:YES
                                                  attributes:nil
                                                       error:&err];
  if (!ok && !s_output_dir_warned) {
    fprintf(stderr, "dx9mt_metal_viewer: failed to create output dir %s (%s)\n",
            dx9mt_output_dir(),
            err ? [[err localizedDescription] UTF8String] : "unknown error");
    s_output_dir_warned = 1;
  }
}

static void build_output_path(char *out, size_t out_size, const char *name) {
  int n;

  if (!out || out_size == 0) {
    return;
  }

  n = snprintf(out, out_size, "%s/%s", dx9mt_output_dir(), name);
  if (n < 0 || (size_t)n >= out_size) {
    snprintf(out, out_size, "%s", name);
  }
}

static NSString *const s_shader_source =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "\n"
     "/* Overlay bar (from RB1) */\n"
     "struct OverlayConstants {\n"
     "  float4 color;\n"
     "  float4 rect;\n"
     "};\n"
     "struct OverlayOut {\n"
     "  float4 position [[position]];\n"
     "};\n"
     "vertex OverlayOut overlay_vertex(\n"
     "    uint vid [[vertex_id]],\n"
     "    constant OverlayConstants &c [[buffer(0)]]) {\n"
     "  float2 corners[4] = {\n"
     "    float2(c.rect.x,            c.rect.y),\n"
     "    float2(c.rect.x + c.rect.z, c.rect.y),\n"
     "    float2(c.rect.x,            c.rect.y + c.rect.w),\n"
     "    float2(c.rect.x + c.rect.z, c.rect.y + c.rect.w),\n"
     "  };\n"
     "  OverlayOut out;\n"
     "  out.position = float4(corners[vid], 0.0, 1.0);\n"
     "  return out;\n"
     "}\n"
     "fragment float4 overlay_fragment(\n"
     "    OverlayOut in [[stage_in]],\n"
     "    constant OverlayConstants &c [[buffer(0)]]) {\n"
     "  return c.color;\n"
     "}\n"
     "\n"
     "/* RB3 geometry shader with WVP matrix from VS constants.\n"
     " * D3D9 SM3.0 vertex shaders typically store the world-view-\n"
     " * projection matrix in constants c0-c3 (4 rows of float4).\n"
     " * We extract this matrix and apply it to the vertex position.\n"
     " * This gives correctly projected geometry without full D3D9\n"
     " * shader bytecode translation.\n"
     " */\n"
     "struct GeoIn {\n"
     "  float3 position [[attribute(0)]];\n"
     "  float4 color    [[attribute(1)]];\n"
     "  float2 texcoord [[attribute(2)]];\n"
     "};\n"
     "struct GeoOut {\n"
     "  float4 position [[position]];\n"
     "  float4 color;\n"
     "  float2 texcoord;\n"
     "};\n"
     "vertex GeoOut geo_vertex(\n"
     "    GeoIn in [[stage_in]],\n"
     "    constant float4 *vs_constants [[buffer(1)]]) {\n"
     "  GeoOut out;\n"
     "  /* Extract WVP matrix from VS constants c0-c3.\n"
     "     D3D9 stores matrices row-major, Metal expects column-major.\n"
     "     float4x4 constructor takes columns, so we transpose. */\n"
     "  float4x4 wvp = float4x4(\n"
     "    float4(vs_constants[0].x, vs_constants[1].x, vs_constants[2].x, vs_constants[3].x),\n"
     "    float4(vs_constants[0].y, vs_constants[1].y, vs_constants[2].y, vs_constants[3].y),\n"
     "    float4(vs_constants[0].z, vs_constants[1].z, vs_constants[2].z, vs_constants[3].z),\n"
     "    float4(vs_constants[0].w, vs_constants[1].w, vs_constants[2].w, vs_constants[3].w));\n"
     "  out.position = wvp * float4(in.position, 1.0);\n"
     "  out.color = in.color;\n"
     "  out.texcoord = in.texcoord;\n"
     "  return out;\n"
     "}\n"
     "struct GeoTexParams {\n"
     "  uint use_vertex_color;\n"
     "  uint use_stage0_combiner;\n"
     "  uint alpha_only;\n"
     "  uint force_alpha_one;\n"
     "  uint alpha_test_enable;\n"
     "  float alpha_ref;\n"
     "  uint alpha_func;\n"
     "  uint color_op;\n"
     "  uint color_arg1;\n"
     "  uint color_arg2;\n"
     "  uint alpha_op;\n"
     "  uint alpha_arg1;\n"
     "  uint alpha_arg2;\n"
     "  uint texture_factor_argb;\n"
     "  uint has_pixel_shader;\n"
     "  uint fog_enable;\n"
     "  float4 ps_c0;\n"
     "  uint fog_mode;\n"
     "  float fog_start;\n"
     "  float fog_end;\n"
     "  float fog_density;\n"
     "  float4 fog_color;\n"
     "};\n"
     "static inline float4 apply_fog(float4 color, float depth,\n"
     "    uint fog_enable, uint fog_mode, float fog_start, float fog_end,\n"
     "    float fog_density, float4 fog_color) {\n"
     "  if (fog_enable == 0u) return color;\n"
     "  float f = 1.0;\n"
     "  if (fog_mode == 3u) {\n"
     "    f = saturate((fog_end - depth) / (fog_end - fog_start));\n"
     "  } else if (fog_mode == 1u) {\n"
     "    f = exp(-fog_density * depth);\n"
     "  } else if (fog_mode == 2u) {\n"
     "    f = exp(-(fog_density * depth) * (fog_density * depth));\n"
     "  }\n"
     "  color.rgb = mix(fog_color.rgb, color.rgb, f);\n"
     "  return color;\n"
     "}\n"
     "static inline float4 d3d_decode_tfactor(uint argb) {\n"
     "  float a = float((argb >> 24) & 0xFFu) / 255.0;\n"
     "  float r = float((argb >> 16) & 0xFFu) / 255.0;\n"
     "  float g = float((argb >> 8) & 0xFFu) / 255.0;\n"
     "  float b = float(argb & 0xFFu) / 255.0;\n"
     "  return float4(r, g, b, a);\n"
     "}\n"
     "static inline float3 d3d_clamp01(float3 v) {\n"
     "  return clamp(v, float3(0.0), float3(1.0));\n"
     "}\n"
     "static inline float d3d_clamp01(float v) {\n"
     "  return clamp(v, 0.0, 1.0);\n"
     "}\n"
     "static inline bool d3d_alpha_test_pass(float a, float ref, uint func) {\n"
     "  constexpr float eps = 1.0 / 255.0;\n"
     "  switch (func) {\n"
     "  case 1u: return false;\n"
     "  case 2u: return a < ref;\n"
     "  case 3u: return fabs(a - ref) <= eps;\n"
     "  case 4u: return (a < ref) || (fabs(a - ref) <= eps);\n"
     "  case 5u: return a > ref;\n"
     "  case 6u: return fabs(a - ref) > eps;\n"
     "  case 7u: return (a > ref) || (fabs(a - ref) <= eps);\n"
     "  case 8u: return true;\n"
     "  default: return true;\n"
     "  }\n"
     "}\n"
     "static inline float4 d3d_resolve_source(\n"
     "    uint arg,\n"
     "    float4 current,\n"
     "    float4 diffuse,\n"
     "    float4 texel,\n"
     "    float4 tfactor) {\n"
     "  float4 v = current;\n"
     "  switch (arg & 0x0Fu) {\n"
     "  case 0u:\n"
     "    v = diffuse;\n"
     "    break;\n"
     "  case 1u:\n"
     "    v = current;\n"
     "    break;\n"
     "  case 2u:\n"
     "    v = texel;\n"
     "    break;\n"
     "  case 3u:\n"
     "    v = tfactor;\n"
     "    break;\n"
     "  default:\n"
     "    v = current;\n"
     "    break;\n"
     "  }\n"
     "  if ((arg & 0x10u) != 0u) {\n"
     "    v = float4(1.0) - v;\n"
     "  }\n"
     "  return v;\n"
     "}\n"
     "static inline float3 d3d_resolve_color(\n"
     "    uint arg,\n"
     "    float4 current,\n"
     "    float4 diffuse,\n"
     "    float4 texel,\n"
     "    float4 tfactor) {\n"
     "  float4 v = d3d_resolve_source(arg, current, diffuse, texel, tfactor);\n"
     "  if ((arg & 0x20u) != 0u) {\n"
     "    return float3(v.a);\n"
     "  }\n"
     "  return v.rgb;\n"
     "}\n"
     "static inline float d3d_resolve_alpha(\n"
     "    uint arg,\n"
     "    float4 current,\n"
     "    float4 diffuse,\n"
     "    float4 texel,\n"
     "    float4 tfactor) {\n"
     "  float4 v = d3d_resolve_source(arg, current, diffuse, texel, tfactor);\n"
     "  return v.a;\n"
     "}\n"
     "static inline float3 d3d_color_op(\n"
     "    uint op,\n"
     "    float3 arg1,\n"
     "    float3 arg2,\n"
     "    float4 current,\n"
     "    float4 diffuse,\n"
     "    float4 texel,\n"
     "    float4 tfactor) {\n"
     "  switch (op) {\n"
     "  case 1u:\n"
     "    return current.rgb;\n"
     "  case 2u:\n"
     "    return d3d_clamp01(arg1);\n"
     "  case 3u:\n"
     "    return d3d_clamp01(arg2);\n"
     "  case 4u:\n"
     "    return d3d_clamp01(arg1 * arg2);\n"
     "  case 5u:\n"
     "    return d3d_clamp01((arg1 * arg2) * 2.0);\n"
     "  case 6u:\n"
     "    return d3d_clamp01((arg1 * arg2) * 4.0);\n"
     "  case 7u:\n"
     "    return d3d_clamp01(arg1 + arg2);\n"
     "  case 8u:\n"
     "    return d3d_clamp01(arg1 + arg2 - 0.5);\n"
     "  case 9u:\n"
     "    return d3d_clamp01((arg1 + arg2 - 0.5) * 2.0);\n"
     "  case 10u:\n"
     "    return d3d_clamp01(arg1 - arg2);\n"
     "  case 11u:\n"
     "    return d3d_clamp01(arg1 + arg2 * (1.0 - arg1));\n"
     "  case 12u:\n"
     "    return d3d_clamp01(mix(arg2, arg1, diffuse.a));\n"
     "  case 13u:\n"
     "    return d3d_clamp01(mix(arg2, arg1, texel.a));\n"
     "  case 14u:\n"
     "    return d3d_clamp01(mix(arg2, arg1, tfactor.a));\n"
     "  case 15u:\n"
     "    return d3d_clamp01(arg1 + arg2 * (1.0 - texel.a));\n"
     "  case 16u:\n"
     "    return d3d_clamp01(mix(arg2, arg1, current.a));\n"
     "  case 17u:\n"
     "    return d3d_clamp01(arg1 * arg2);\n"
     "  default:\n"
     "    return d3d_clamp01(arg1 * arg2);\n"
     "  }\n"
     "}\n"
     "static inline float d3d_alpha_op(\n"
     "    uint op,\n"
     "    float arg1,\n"
     "    float arg2,\n"
     "    float4 current,\n"
     "    float4 diffuse,\n"
     "    float4 texel,\n"
     "    float4 tfactor) {\n"
     "  switch (op) {\n"
     "  case 1u:\n"
     "    return current.a;\n"
     "  case 2u:\n"
     "    return d3d_clamp01(arg1);\n"
     "  case 3u:\n"
     "    return d3d_clamp01(arg2);\n"
     "  case 4u:\n"
     "    return d3d_clamp01(arg1 * arg2);\n"
     "  case 5u:\n"
     "    return d3d_clamp01((arg1 * arg2) * 2.0);\n"
     "  case 6u:\n"
     "    return d3d_clamp01((arg1 * arg2) * 4.0);\n"
     "  case 7u:\n"
     "    return d3d_clamp01(arg1 + arg2);\n"
     "  case 8u:\n"
     "    return d3d_clamp01(arg1 + arg2 - 0.5);\n"
     "  case 9u:\n"
     "    return d3d_clamp01((arg1 + arg2 - 0.5) * 2.0);\n"
     "  case 10u:\n"
     "    return d3d_clamp01(arg1 - arg2);\n"
     "  case 11u:\n"
     "    return d3d_clamp01(arg1 + arg2 * (1.0 - arg1));\n"
     "  case 12u:\n"
     "    return d3d_clamp01(mix(arg2, arg1, diffuse.a));\n"
     "  case 13u:\n"
     "    return d3d_clamp01(mix(arg2, arg1, texel.a));\n"
     "  case 14u:\n"
     "    return d3d_clamp01(mix(arg2, arg1, tfactor.a));\n"
     "  case 15u:\n"
     "    return d3d_clamp01(arg1 + arg2 * (1.0 - texel.a));\n"
     "  case 16u:\n"
     "    return d3d_clamp01(mix(arg2, arg1, current.a));\n"
     "  case 17u:\n"
     "    return d3d_clamp01(arg1 * arg2);\n"
     "  default:\n"
     "    return d3d_clamp01(arg1 * arg2);\n"
     "  }\n"
     "}\n"
     "fragment float4 geo_fragment(\n"
     "    GeoOut in [[stage_in]],\n"
     "    constant GeoTexParams &p [[buffer(0)]]) {\n"
     "  float4 diffuse = (p.use_vertex_color != 0u) ? in.color : float4(1.0);\n"
     "  float4 out_color;\n"
     "  if (p.use_stage0_combiner != 0u) {\n"
     "    float4 current = diffuse;\n"
     "    float4 texel = float4(1.0);\n"
     "    float4 tfactor = d3d_decode_tfactor(p.texture_factor_argb);\n"
     "    float3 c1 = d3d_resolve_color(p.color_arg1, current, diffuse, texel, tfactor);\n"
     "    float3 c2 = d3d_resolve_color(p.color_arg2, current, diffuse, texel, tfactor);\n"
     "    float a1 = d3d_resolve_alpha(p.alpha_arg1, current, diffuse, texel, tfactor);\n"
     "    float a2 = d3d_resolve_alpha(p.alpha_arg2, current, diffuse, texel, tfactor);\n"
     "    out_color.rgb = d3d_color_op(p.color_op, c1, c2, current, diffuse, texel, tfactor);\n"
     "    out_color.a = d3d_alpha_op(p.alpha_op, a1, a2, current, diffuse, texel, tfactor);\n"
     "  } else if (p.has_pixel_shader != 0u) {\n"
     "    out_color = p.ps_c0;\n"
     "  } else {\n"
     "    out_color = diffuse;\n"
     "  }\n"
     "  if (p.alpha_test_enable != 0u &&\n"
     "      !d3d_alpha_test_pass(out_color.a, p.alpha_ref, p.alpha_func)) {\n"
     "    discard_fragment();\n"
     "  }\n"
     "  out_color = apply_fog(out_color, in.position.z,\n"
     "    p.fog_enable, p.fog_mode, p.fog_start, p.fog_end,\n"
     "    p.fog_density, p.fog_color);\n"
     "  return out_color;\n"
     "}\n"
     "fragment float4 geo_fragment_textured(\n"
     "    GeoOut in [[stage_in]],\n"
     "    texture2d<float> tex [[texture(0)]],\n"
     "    sampler samp [[sampler(0)]],\n"
     "    constant GeoTexParams &p [[buffer(0)]]) {\n"
     "  float4 t = tex.sample(samp, in.texcoord);\n"
     "  if (p.alpha_only != 0u) {\n"
     "    t = float4(1.0, 1.0, 1.0, t.r);\n"
     "  }\n"
     "  if (p.force_alpha_one != 0u) {\n"
     "    t.a = 1.0;\n"
     "  }\n"
     "  float4 diffuse = (p.use_vertex_color != 0u) ? in.color : float4(1.0);\n"
     "  float4 out_color;\n"
     "  if (p.use_stage0_combiner != 0u) {\n"
     "    float4 current = diffuse;\n"
     "    float4 tfactor = d3d_decode_tfactor(p.texture_factor_argb);\n"
     "    float3 c1 = d3d_resolve_color(p.color_arg1, current, diffuse, t, tfactor);\n"
     "    float3 c2 = d3d_resolve_color(p.color_arg2, current, diffuse, t, tfactor);\n"
     "    float a1 = d3d_resolve_alpha(p.alpha_arg1, current, diffuse, t, tfactor);\n"
     "    float a2 = d3d_resolve_alpha(p.alpha_arg2, current, diffuse, t, tfactor);\n"
     "    out_color.rgb = d3d_color_op(p.color_op, c1, c2, current, diffuse, t, tfactor);\n"
     "    out_color.a = d3d_alpha_op(p.alpha_op, a1, a2, current, diffuse, t, tfactor);\n"
     "  } else if (p.has_pixel_shader != 0u) {\n"
     "    out_color = t * p.ps_c0;\n"
     "  } else {\n"
     "    out_color = diffuse * t;\n"
     "  }\n"
     "  if (p.alpha_test_enable != 0u &&\n"
     "      !d3d_alpha_test_pass(out_color.a, p.alpha_ref, p.alpha_func)) {\n"
     "    discard_fragment();\n"
     "  }\n"
     "  out_color = apply_fog(out_color, in.position.z,\n"
     "    p.fog_enable, p.fog_mode, p.fog_start, p.fog_end,\n"
     "    p.fog_density, p.fog_color);\n"
     "  return out_color;\n"
     "}\n";

static MTLVertexFormat decl_type_to_mtl(uint8_t type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1:
    return MTLVertexFormatFloat;
  case D3DDECLTYPE_FLOAT2:
    return MTLVertexFormatFloat2;
  case D3DDECLTYPE_FLOAT3:
    return MTLVertexFormatFloat3;
  case D3DDECLTYPE_FLOAT4:
    return MTLVertexFormatFloat4;
  case D3DDECLTYPE_D3DCOLOR:
    return MTLVertexFormatUChar4Normalized_BGRA;
  case D3DDECLTYPE_UBYTE4:
    return MTLVertexFormatUChar4;
  case D3DDECLTYPE_UBYTE4N:
    return MTLVertexFormatUChar4Normalized;
  case D3DDECLTYPE_SHORT2:
    return MTLVertexFormatShort2;
  case D3DDECLTYPE_SHORT4:
    return MTLVertexFormatShort4;
  case D3DDECLTYPE_SHORT2N:
    return MTLVertexFormatShort2Normalized;
  case D3DDECLTYPE_SHORT4N:
    return MTLVertexFormatShort4Normalized;
  case D3DDECLTYPE_USHORT2N:
    return MTLVertexFormatUShort2Normalized;
  case D3DDECLTYPE_USHORT4N:
    return MTLVertexFormatUShort4Normalized;
  case D3DDECLTYPE_FLOAT16_2:
    return MTLVertexFormatHalf2;
  case D3DDECLTYPE_FLOAT16_4:
    return MTLVertexFormatHalf4;
  default:
    return MTLVertexFormatFloat3;
  }
}

static MTLPrimitiveType d3d_prim_to_mtl(uint32_t d3d_type) {
  switch (d3d_type) {
  case D3DPT_TRIANGLELIST:
    return MTLPrimitiveTypeTriangle;
  case D3DPT_TRIANGLESTRIP:
    return MTLPrimitiveTypeTriangleStrip;
  default:
    return MTLPrimitiveTypeTriangle;
  }
}

static uint32_t d3d_index_count(uint32_t prim_type, uint32_t prim_count) {
  switch (prim_type) {
  case D3DPT_TRIANGLELIST:
    return prim_count * 3;
  case D3DPT_TRIANGLESTRIP:
    return prim_count + 2;
  case D3DPT_TRIANGLEFAN:
    return prim_count + 2;
  default:
    return prim_count * 3;
  }
}

static MTLPixelFormat d3d_texture_format_to_mtl(uint32_t format) {
  switch (format) {
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8:
    return MTLPixelFormatBGRA8Unorm;
  case D3DFMT_A8:
    return MTLPixelFormatR8Unorm;
  case D3DFMT_DXT1:
    return MTLPixelFormatBC1_RGBA;
  case D3DFMT_DXT3:
    return MTLPixelFormatBC2_RGBA;
  case D3DFMT_DXT5:
    return MTLPixelFormatBC3_RGBA;
  default:
    return MTLPixelFormatInvalid;
  }
}

static BOOL d3d_texture_format_is_compressed(uint32_t format) {
  return format == D3DFMT_DXT1 || format == D3DFMT_DXT3 ||
         format == D3DFMT_DXT5;
}

static uint32_t d3d_texture_min_row_pitch(uint32_t format, uint32_t width) {
  if (format == D3DFMT_DXT1) {
    uint32_t blocks = (width + 3u) / 4u;
    return (blocks == 0 ? 1u : blocks) * 8u;
  }
  if (format == D3DFMT_DXT3 || format == D3DFMT_DXT5) {
    uint32_t blocks = (width + 3u) / 4u;
    return (blocks == 0 ? 1u : blocks) * 16u;
  }
  if (format == D3DFMT_A8) {
    return width;
  }
  return width * 4u;
}

static uint64_t hash_decl_key(uint32_t stride,
                              const dx9mt_d3d_vertex_element *elems,
                              uint16_t elem_count, int textured,
                              uint32_t blend_enable, uint32_t src_blend,
                              uint32_t dst_blend, uint32_t blend_op,
                              uint32_t color_write_mask) {
  uint64_t hash = 1469598103934665603ull;

  hash ^= (uint64_t)stride;
  hash *= 1099511628211ull;
  hash ^= (uint64_t)elem_count;
  hash *= 1099511628211ull;
  hash ^= (uint64_t)(textured ? 1u : 0u);
  hash *= 1099511628211ull;
  hash ^= (uint64_t)(blend_enable ? 1u : 0u);
  hash *= 1099511628211ull;
  hash ^= (uint64_t)src_blend;
  hash *= 1099511628211ull;
  hash ^= (uint64_t)dst_blend;
  hash *= 1099511628211ull;
  hash ^= (uint64_t)blend_op;
  hash *= 1099511628211ull;
  hash ^= (uint64_t)color_write_mask;
  hash *= 1099511628211ull;

  if (!elems || elem_count == 0) {
    return hash;
  }
  for (uint16_t i = 0; i < elem_count; ++i) {
    const uint8_t *bytes = (const uint8_t *)&elems[i];
    for (uint32_t j = 0; j < sizeof(dx9mt_d3d_vertex_element); ++j) {
      hash ^= (uint64_t)bytes[j];
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

static void scan_decl_semantics(const dx9mt_d3d_vertex_element *elems,
                                uint16_t elem_count, int *has_position,
                                int *has_color, int *has_texcoord,
                                int *has_positiont) {
  if (has_position) {
    *has_position = 0;
  }
  if (has_color) {
    *has_color = 0;
  }
  if (has_texcoord) {
    *has_texcoord = 0;
  }
  if (has_positiont) {
    *has_positiont = 0;
  }
  if (!elems || elem_count == 0) {
    return;
  }

  for (uint16_t i = 0; i < elem_count; ++i) {
    if (elems[i].usage == D3DDECLUSAGE_POSITION && elems[i].usage_index == 0) {
      if (has_position) {
        *has_position = 1;
      }
    } else if (elems[i].usage == D3DDECLUSAGE_POSITIONT &&
               elems[i].usage_index == 0) {
      /* Pre-transformed vertices count as position for PSO purposes. */
      if (has_position) {
        *has_position = 1;
      }
      if (has_positiont) {
        *has_positiont = 1;
      }
    } else if (elems[i].usage == D3DDECLUSAGE_COLOR &&
               elems[i].usage_index == 0) {
      if (has_color) {
        *has_color = 1;
      }
    } else if (elems[i].usage == D3DDECLUSAGE_TEXCOORD &&
               elems[i].usage_index == 0) {
      if (has_texcoord) {
        *has_texcoord = 1;
      }
    }
  }
}

/*
 * Convert an FVF bitmask to a synthetic vertex element array (viewer fallback).
 * Mirrors the frontend dx9mt_fvf_to_vertex_elements() using viewer-local types.
 */
static uint16_t fvf_to_elements(uint32_t fvf, dx9mt_d3d_vertex_element *elems,
                                uint16_t max_elems) {
  uint16_t count = 0;
  uint16_t offset = 0;
  uint32_t pos_type;
  uint32_t tex_count;
  uint32_t blend_count;

  if (!elems || max_elems == 0 || fvf == 0) {
    return 0;
  }

  pos_type = fvf & D3DFVF_POSITION_MASK;

  if (pos_type == D3DFVF_XYZRHW) {
    if (count >= max_elems) return count;
    elems[count].stream = 0; elems[count].offset = offset;
    elems[count].type = D3DDECLTYPE_FLOAT4; elems[count].method = 0;
    elems[count].usage = D3DDECLUSAGE_POSITIONT; elems[count].usage_index = 0;
    count++; offset += 16;
  } else if (pos_type == D3DFVF_XYZ || pos_type == D3DFVF_XYZW) {
    if (count >= max_elems) return count;
    elems[count].stream = 0; elems[count].offset = offset;
    elems[count].type = (pos_type == D3DFVF_XYZW) ? D3DDECLTYPE_FLOAT4
                                                   : D3DDECLTYPE_FLOAT3;
    elems[count].method = 0;
    elems[count].usage = D3DDECLUSAGE_POSITION; elems[count].usage_index = 0;
    count++; offset += (pos_type == D3DFVF_XYZW) ? 16 : 12;
  } else if (pos_type >= D3DFVF_XYZB1 && pos_type <= D3DFVF_XYZB5) {
    if (count >= max_elems) return count;
    elems[count].stream = 0; elems[count].offset = offset;
    elems[count].type = D3DDECLTYPE_FLOAT3; elems[count].method = 0;
    elems[count].usage = D3DDECLUSAGE_POSITION; elems[count].usage_index = 0;
    count++; offset += 12;
    blend_count = (pos_type - D3DFVF_XYZ) / 2;
    offset += (uint16_t)(blend_count * 4);
  }

  if (fvf & D3DFVF_NORMAL) {
    if (count < max_elems) {
      elems[count].stream = 0; elems[count].offset = offset;
      elems[count].type = D3DDECLTYPE_FLOAT3; elems[count].method = 0;
      elems[count].usage = D3DDECLUSAGE_NORMAL; elems[count].usage_index = 0;
      count++;
    }
    offset += 12;
  }

  if (fvf & D3DFVF_PSIZE) { offset += 4; }

  if (fvf & D3DFVF_DIFFUSE) {
    if (count < max_elems) {
      elems[count].stream = 0; elems[count].offset = offset;
      elems[count].type = D3DDECLTYPE_D3DCOLOR; elems[count].method = 0;
      elems[count].usage = D3DDECLUSAGE_COLOR; elems[count].usage_index = 0;
      count++;
    }
    offset += 4;
  }

  if (fvf & D3DFVF_SPECULAR) {
    if (count < max_elems) {
      elems[count].stream = 0; elems[count].offset = offset;
      elems[count].type = D3DDECLTYPE_D3DCOLOR; elems[count].method = 0;
      elems[count].usage = D3DDECLUSAGE_COLOR; elems[count].usage_index = 1;
      count++;
    }
    offset += 4;
  }

  tex_count = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
  for (uint32_t i = 0; i < tex_count; ++i) {
    uint32_t fmt_bits = (fvf >> (16 + i * 2)) & 0x3;
    uint8_t decl_type; uint16_t size;
    switch (fmt_bits) {
    case 0:  decl_type = D3DDECLTYPE_FLOAT2; size = 8;  break;
    case 1:  decl_type = D3DDECLTYPE_FLOAT3; size = 12; break;
    case 2:  decl_type = D3DDECLTYPE_FLOAT4; size = 16; break;
    default: decl_type = D3DDECLTYPE_FLOAT1; size = 4;  break;
    }
    if (count < max_elems) {
      elems[count].stream = 0; elems[count].offset = offset;
      elems[count].type = decl_type; elems[count].method = 0;
      elems[count].usage = D3DDECLUSAGE_TEXCOORD;
      elems[count].usage_index = (uint8_t)i;
      count++;
    }
    offset += size;
  }

  return count;
}

static MTLSamplerAddressMode d3d_address_to_mtl(uint32_t mode) {
  switch (mode) {
  case D3DTADDRESS_WRAP:
    return MTLSamplerAddressModeRepeat;
  case D3DTADDRESS_MIRROR:
    return MTLSamplerAddressModeMirrorRepeat;
  case D3DTADDRESS_CLAMP:
    return MTLSamplerAddressModeClampToEdge;
  case D3DTADDRESS_BORDER:
    return MTLSamplerAddressModeClampToBorderColor;
  case D3DTADDRESS_MIRRORONCE:
    return MTLSamplerAddressModeMirrorClampToEdge;
  default:
    return MTLSamplerAddressModeRepeat;
  }
}

static MTLSamplerMinMagFilter d3d_filter_to_mtl_minmag(uint32_t filter) {
  if (filter == D3DTEXF_LINEAR || filter == D3DTEXF_ANISOTROPIC) {
    return MTLSamplerMinMagFilterLinear;
  }
  return MTLSamplerMinMagFilterNearest;
}

static MTLBlendFactor d3d_blend_to_mtl(uint32_t blend, int is_source) {
  switch (blend) {
  case D3DBLEND_ZERO:
    return MTLBlendFactorZero;
  case D3DBLEND_ONE:
    return MTLBlendFactorOne;
  case D3DBLEND_SRCCOLOR:
    return MTLBlendFactorSourceColor;
  case D3DBLEND_INVSRCCOLOR:
    return MTLBlendFactorOneMinusSourceColor;
  case D3DBLEND_SRCALPHA:
  case D3DBLEND_BOTHSRCALPHA:
    return MTLBlendFactorSourceAlpha;
  case D3DBLEND_INVSRCALPHA:
  case D3DBLEND_BOTHINVSRCALPHA:
    return MTLBlendFactorOneMinusSourceAlpha;
  case D3DBLEND_DESTALPHA:
    return MTLBlendFactorDestinationAlpha;
  case D3DBLEND_INVDESTALPHA:
    return MTLBlendFactorOneMinusDestinationAlpha;
  case D3DBLEND_DESTCOLOR:
    return MTLBlendFactorDestinationColor;
  case D3DBLEND_INVDESTCOLOR:
    return MTLBlendFactorOneMinusDestinationColor;
  case D3DBLEND_SRCALPHASAT:
    return is_source ? MTLBlendFactorSourceAlphaSaturated : MTLBlendFactorOne;
  default:
    return is_source ? MTLBlendFactorSourceAlpha
                     : MTLBlendFactorOneMinusSourceAlpha;
  }
}

static MTLCompareFunction d3d_cmp_to_mtl(uint32_t func) {
  switch (func) {
  case D3DCMP_NEVER:        return MTLCompareFunctionNever;
  case D3DCMP_LESS:         return MTLCompareFunctionLess;
  case D3DCMP_EQUAL:        return MTLCompareFunctionEqual;
  case D3DCMP_LESSEQUAL:    return MTLCompareFunctionLessEqual;
  case D3DCMP_GREATER:      return MTLCompareFunctionGreater;
  case D3DCMP_NOTEQUAL:     return MTLCompareFunctionNotEqual;
  case D3DCMP_GREATEREQUAL: return MTLCompareFunctionGreaterEqual;
  case D3DCMP_ALWAYS:       return MTLCompareFunctionAlways;
  default:                  return MTLCompareFunctionAlways;
  }
}

static id<MTLTexture> depth_texture_for_rt(uint32_t rt_id, uint32_t w,
                                           uint32_t h) {
  NSNumber *key = @(rt_id);
  id<MTLTexture> cached = [s_depth_texture_cache objectForKey:key];
  if (cached && cached.width == w && cached.height == h) {
    return cached;
  }
  MTLTextureDescriptor *desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                   width:w
                                  height:h
                               mipmapped:NO];
  desc.usage = MTLTextureUsageRenderTarget;
  desc.storageMode = MTLStorageModePrivate;
  id<MTLTexture> tex = [s_device newTextureWithDescriptor:desc];
  if (tex) {
    [s_depth_texture_cache setObject:tex forKey:key];
  }
  return tex;
}

static id<MTLTexture> ensure_drawable_depth_texture(uint32_t w, uint32_t h) {
  if (s_drawable_depth_texture && s_drawable_depth_w == w &&
      s_drawable_depth_h == h) {
    return s_drawable_depth_texture;
  }
  MTLTextureDescriptor *desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                   width:w
                                  height:h
                               mipmapped:NO];
  desc.usage = MTLTextureUsageRenderTarget;
  desc.storageMode = MTLStorageModePrivate;
  s_drawable_depth_texture = [s_device newTextureWithDescriptor:desc];
  s_drawable_depth_w = w;
  s_drawable_depth_h = h;
  return s_drawable_depth_texture;
}

static MTLCullMode d3d_cull_to_mtl(uint32_t cull_mode) {
  switch (cull_mode) {
  case 1: /* D3DCULL_NONE */
    return MTLCullModeNone;
  case 2: /* D3DCULL_CW */
    return MTLCullModeFront;
  case 3: /* D3DCULL_CCW */
    return MTLCullModeBack;
  default:
    return MTLCullModeBack;
  }
}

static MTLBlendOperation d3d_blendop_to_mtl(uint32_t op) {
  switch (op) {
  case 1:  return MTLBlendOperationAdd;            /* D3DBLENDOP_ADD */
  case 2:  return MTLBlendOperationSubtract;       /* D3DBLENDOP_SUBTRACT */
  case 3:  return MTLBlendOperationReverseSubtract;/* D3DBLENDOP_REVSUBTRACT */
  case 4:  return MTLBlendOperationMin;            /* D3DBLENDOP_MIN */
  case 5:  return MTLBlendOperationMax;            /* D3DBLENDOP_MAX */
  default: return MTLBlendOperationAdd;
  }
}

static MTLColorWriteMask d3d_writemask_to_mtl(uint32_t mask) {
  MTLColorWriteMask m = MTLColorWriteMaskNone;
  if (mask & 0x1) m |= MTLColorWriteMaskRed;
  if (mask & 0x2) m |= MTLColorWriteMaskGreen;
  if (mask & 0x4) m |= MTLColorWriteMaskBlue;
  if (mask & 0x8) m |= MTLColorWriteMaskAlpha;
  return m;
}

static MTLStencilOperation d3d_stencilop_to_mtl(uint32_t op) {
  switch (op) {
  case 1:  return MTLStencilOperationKeep;
  case 2:  return MTLStencilOperationZero;
  case 3:  return MTLStencilOperationReplace;
  case 4:  return MTLStencilOperationIncrementClamp;
  case 5:  return MTLStencilOperationDecrementClamp;
  case 6:  return MTLStencilOperationInvert;
  case 7:  return MTLStencilOperationIncrementWrap;
  case 8:  return MTLStencilOperationDecrementWrap;
  default: return MTLStencilOperationKeep;
  }
}

static id<MTLDepthStencilState>
depth_stencil_state_for_draw(const volatile dx9mt_metal_ipc_draw *d) {
  uint32_t zenable = d->rs_zenable;
  uint32_t zwrite = d->rs_zwriteenable;
  uint32_t zfunc = d->rs_zfunc;
  uint32_t stencil_en = d->rs_stencilenable;
  uint64_t key_val = (uint64_t)zenable | ((uint64_t)zwrite << 8) |
                     ((uint64_t)zfunc << 16);
  if (stencil_en) {
    key_val ^= ((uint64_t)d->rs_stencilfunc << 24) |
               ((uint64_t)d->rs_stencilpass << 28) |
               ((uint64_t)d->rs_stencilfail << 32) |
               ((uint64_t)d->rs_stencilzfail << 36) |
               ((uint64_t)d->rs_stencilmask << 40) |
               ((uint64_t)(d->rs_stencilwritemask & 0xFF) << 48);
  }
  NSNumber *key = @(key_val);
  id<MTLDepthStencilState> cached = [s_depth_stencil_cache objectForKey:key];
  if (cached) {
    return cached;
  }
  MTLDepthStencilDescriptor *desc = [[MTLDepthStencilDescriptor alloc] init];
  if (zenable) {
    desc.depthCompareFunction = d3d_cmp_to_mtl(zfunc);
    desc.depthWriteEnabled = (zwrite != 0) ? YES : NO;
  } else {
    desc.depthCompareFunction = MTLCompareFunctionAlways;
    desc.depthWriteEnabled = NO;
  }
  if (stencil_en) {
    MTLStencilDescriptor *sd = [[MTLStencilDescriptor alloc] init];
    sd.stencilCompareFunction = d3d_cmp_to_mtl(d->rs_stencilfunc);
    sd.stencilFailureOperation = d3d_stencilop_to_mtl(d->rs_stencilfail);
    sd.depthFailureOperation = d3d_stencilop_to_mtl(d->rs_stencilzfail);
    sd.depthStencilPassOperation = d3d_stencilop_to_mtl(d->rs_stencilpass);
    sd.readMask = d->rs_stencilmask;
    sd.writeMask = d->rs_stencilwritemask;
    desc.frontFaceStencil = sd;
    desc.backFaceStencil = sd;
  }
  id<MTLDepthStencilState> state =
      [s_device newDepthStencilStateWithDescriptor:desc];
  if (state) {
    [s_depth_stencil_cache setObject:state forKey:key];
  }
  return state;
}

static MTLSamplerMipFilter d3d_filter_to_mtl_mip(uint32_t filter) {
  if (filter == D3DTEXF_POINT) {
    return MTLSamplerMipFilterNearest;
  }
  if (filter == D3DTEXF_LINEAR || filter == D3DTEXF_ANISOTROPIC) {
    return MTLSamplerMipFilterLinear;
  }
  return MTLSamplerMipFilterNotMipmapped;
}

static id<MTLSamplerState>
sampler_state_for_draw_stage(const volatile dx9mt_metal_ipc_draw *draw,
                             uint32_t stage) {
  uint64_t key_value;
  NSNumber *key;
  id<MTLSamplerState> state;
  MTLSamplerDescriptor *desc;
  uint32_t min_filter;
  uint32_t mag_filter;
  uint32_t mip_filter;
  uint32_t address_u;
  uint32_t address_v;
  uint32_t address_w;

  if (!draw || !s_sampler_cache) {
    return nil;
  }

  min_filter = draw->sampler_min_filter[stage];
  mag_filter = draw->sampler_mag_filter[stage];
  mip_filter = draw->sampler_mip_filter[stage];
  address_u = draw->sampler_address_u[stage];
  address_v = draw->sampler_address_v[stage];
  address_w = draw->sampler_address_w[stage];

  key_value = (uint64_t)min_filter | ((uint64_t)mag_filter << 8) |
              ((uint64_t)mip_filter << 16) | ((uint64_t)address_u << 24) |
              ((uint64_t)address_v << 32) | ((uint64_t)address_w << 40);
  key = @(key_value);
  state = [s_sampler_cache objectForKey:key];
  if (state) {
    return state;
  }

  desc = [[MTLSamplerDescriptor alloc] init];
  desc.minFilter = d3d_filter_to_mtl_minmag(min_filter);
  desc.magFilter = d3d_filter_to_mtl_minmag(mag_filter);
  desc.mipFilter = d3d_filter_to_mtl_mip(mip_filter);
  desc.sAddressMode = d3d_address_to_mtl(address_u);
  desc.tAddressMode = d3d_address_to_mtl(address_v);
  desc.rAddressMode = d3d_address_to_mtl(address_w);
  desc.maxAnisotropy =
      (min_filter == D3DTEXF_ANISOTROPIC || mag_filter == D3DTEXF_ANISOTROPIC)
          ? 8
          : 1;

  state = [s_device newSamplerStateWithDescriptor:desc];
  if (state) {
    [s_sampler_cache setObject:state forKey:key];
  }
  return state;
}

static uint64_t pack_render_target_desc(uint32_t width, uint32_t height,
                                        uint32_t format) {
  return (uint64_t)width | ((uint64_t)height << 16) | ((uint64_t)format << 32);
}

static id<MTLTexture>
render_target_texture_for_draw(const volatile dx9mt_metal_ipc_draw *draw) {
  uint32_t render_target_id;
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint64_t packed_desc;
  NSNumber *key;
  NSNumber *cached_desc;
  id<MTLTexture> cached_texture;
  MTLPixelFormat pixel_format;
  MTLTextureDescriptor *desc;
  id<MTLTexture> texture;

  if (!draw || !s_render_target_cache || !s_render_target_desc) {
    return nil;
  }

  render_target_id = draw->render_target_id;
  width = draw->render_target_width;
  height = draw->render_target_height;
  format = draw->render_target_format;
  if (render_target_id == 0 || width == 0 || height == 0) {
    return nil;
  }

  pixel_format = d3d_texture_format_to_mtl(format);
  if (pixel_format == MTLPixelFormatInvalid) {
    return nil;
  }

  packed_desc = pack_render_target_desc(width, height, format);
  key = @(render_target_id);
  cached_desc = [s_render_target_desc objectForKey:key];
  cached_texture = [s_render_target_cache objectForKey:key];
  if (cached_texture && cached_desc &&
      [cached_desc unsignedLongLongValue] == packed_desc) {
    return cached_texture;
  }

  desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel_format
                                                            width:width
                                                           height:height
                                                        mipmapped:NO];
  desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  texture = [s_device newTextureWithDescriptor:desc];
  if (!texture) {
    return cached_texture;
  }

  [s_render_target_cache setObject:texture forKey:key];
  [s_render_target_desc setObject:@(packed_desc) forKey:key];
  return texture;
}

static id<MTLTexture>
texture_for_draw_stage(const volatile unsigned char *ipc_base, uint32_t bulk_off,
                       const volatile dx9mt_metal_ipc_draw *draw,
                       uint32_t stage) {
  uint32_t texture_id;
  uint32_t generation;
  uint32_t format;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  uint32_t upload_offset;
  uint32_t upload_size;
  MTLPixelFormat pixel_format;
  NSNumber *key;
  NSNumber *cached_generation;
  id<MTLTexture> cached_texture;
  id<MTLTexture> texture;
  MTLTextureDescriptor *desc;
  const void *texture_bytes;
  MTLRegion region;

  if (!draw || !s_texture_cache || !s_texture_generation) {
    return nil;
  }

  texture_id = draw->tex_id[stage];
  if (texture_id == 0) {
    return nil;
  }
  generation = draw->tex_generation[stage];
  format = draw->tex_format[stage];
  width = draw->tex_width[stage];
  height = draw->tex_height[stage];
  pitch = draw->tex_pitch[stage];
  upload_offset = draw->tex_bulk_offset[stage];
  upload_size = draw->tex_bulk_size[stage];

  key = @(texture_id);
  texture = [s_texture_rt_overrides objectForKey:key];
  if (texture) {
    return texture;
  }

  cached_texture = [s_texture_cache objectForKey:key];
  cached_generation = [s_texture_generation objectForKey:key];
  if (cached_texture && cached_generation &&
      [cached_generation unsignedIntValue] == generation && upload_size == 0) {
    return cached_texture;
  }
  if (upload_size == 0 || width == 0 || height == 0) {
    return cached_texture;
  }

  pixel_format = d3d_texture_format_to_mtl(format);
  if (pixel_format == MTLPixelFormatInvalid) {
    return cached_texture;
  }

  if (d3d_texture_format_is_compressed(format) && pitch == 0) {
    pitch = d3d_texture_min_row_pitch(format, width);
  }
  if (pitch < d3d_texture_min_row_pitch(format, width)) {
    pitch = d3d_texture_min_row_pitch(format, width);
  }

  desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel_format
                                                            width:width
                                                           height:height
                                                        mipmapped:NO];
  desc.usage = MTLTextureUsageShaderRead;
  texture = [s_device newTextureWithDescriptor:desc];
  if (!texture) {
    return cached_texture;
  }

  texture_bytes = (const void *)(ipc_base + bulk_off + upload_offset);
  region = MTLRegionMake2D(0, 0, width, height);
  [texture replaceRegion:region mipmapLevel:0 withBytes:texture_bytes
             bytesPerRow:pitch];

  [s_texture_cache setObject:texture forKey:key];
  [s_texture_generation setObject:@(generation) forKey:key];
  return texture;
}

/*
 * Create or re-create the geometry PSO for a given vertex stride
 * and declaration. Keep separate untextured/textured variants.
 */
static void ensure_geometry_pso(uint32_t stride,
                                const dx9mt_d3d_vertex_element *elems,
                                uint16_t elem_count, int textured,
                                uint32_t blend_enable, uint32_t src_blend,
                                uint32_t dst_blend, uint32_t blend_op,
                                uint32_t color_write_mask) {
  NSError *error = nil;
  MTLVertexDescriptor *vd;
  MTLRenderPipelineDescriptor *desc;
  uint64_t draw_key;
  int has_position = 0;
  int has_color = 0;
  int has_texcoord = 0;
  uint64_t *key_slot;
  uint32_t eff_src_blend = src_blend;
  uint32_t eff_dst_blend = dst_blend;

  if (eff_src_blend == D3DBLEND_BOTHSRCALPHA) {
    eff_src_blend = D3DBLEND_SRCALPHA;
    eff_dst_blend = D3DBLEND_INVSRCALPHA;
  } else if (eff_src_blend == D3DBLEND_BOTHINVSRCALPHA) {
    eff_src_blend = D3DBLEND_INVSRCALPHA;
    eff_dst_blend = D3DBLEND_SRCALPHA;
  }

  draw_key = hash_decl_key(stride, elems, elem_count, textured, blend_enable,
                           eff_src_blend, eff_dst_blend, blend_op,
                           color_write_mask);
  key_slot = textured ? &s_geometry_textured_pso_key : &s_geometry_pso_key;
  if (textured) {
    if (s_geometry_textured_pso && s_geometry_textured_pso_stride == stride &&
        *key_slot == draw_key) {
      return;
    }
  } else if (s_geometry_pso && s_geometry_pso_stride == stride &&
             *key_slot == draw_key) {
    return;
  }

  if (!s_library) {
    return;
  }

  vd = [[MTLVertexDescriptor alloc] init];

  for (uint16_t i = 0; i < elem_count; ++i) {
    if (elems[i].usage == D3DDECLUSAGE_POSITION && elems[i].usage_index == 0) {
      vd.attributes[0].format = decl_type_to_mtl(elems[i].type);
      vd.attributes[0].offset = elems[i].offset;
      vd.attributes[0].bufferIndex = 0;
      has_position = 1;
    } else if (elems[i].usage == D3DDECLUSAGE_COLOR &&
               elems[i].usage_index == 0) {
      vd.attributes[1].format = decl_type_to_mtl(elems[i].type);
      vd.attributes[1].offset = elems[i].offset;
      vd.attributes[1].bufferIndex = 0;
      has_color = 1;
    } else if (elems[i].usage == D3DDECLUSAGE_TEXCOORD &&
               elems[i].usage_index == 0) {
      vd.attributes[2].format = decl_type_to_mtl(elems[i].type);
      vd.attributes[2].offset = elems[i].offset;
      vd.attributes[2].bufferIndex = 0;
      has_texcoord = 1;
    }
  }

  if (!has_position) {
    /* Can't render without position */
    return;
  }
  if (!has_color) {
    /* Default color attribute: float4 at offset 0 (will read position bytes
     * as color -- produces rainbow debug visual) */
    vd.attributes[1].format = MTLVertexFormatFloat4;
    vd.attributes[1].offset = 0;
    vd.attributes[1].bufferIndex = 0;
  }
  if (!has_texcoord) {
    vd.attributes[2].format = MTLVertexFormatFloat2;
    vd.attributes[2].offset = 0;
    vd.attributes[2].bufferIndex = 0;
  }

  vd.layouts[0].stride = stride;
  vd.layouts[0].stepRate = 1;
  vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

  desc = [[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction = [s_library newFunctionWithName:@"geo_vertex"];
  desc.fragmentFunction =
      [s_library
          newFunctionWithName:(textured ? @"geo_fragment_textured"
                                        : @"geo_fragment")];
  desc.vertexDescriptor = vd;
  desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
  if (blend_enable) {
    MTLBlendFactor src = d3d_blend_to_mtl(eff_src_blend, 1);
    MTLBlendFactor dst = d3d_blend_to_mtl(eff_dst_blend, 0);
    desc.colorAttachments[0].blendingEnabled = YES;
    desc.colorAttachments[0].sourceRGBBlendFactor = src;
    desc.colorAttachments[0].destinationRGBBlendFactor = dst;
    desc.colorAttachments[0].rgbBlendOperation = d3d_blendop_to_mtl(blend_op);
    desc.colorAttachments[0].sourceAlphaBlendFactor = src;
    desc.colorAttachments[0].destinationAlphaBlendFactor = dst;
    desc.colorAttachments[0].alphaBlendOperation = d3d_blendop_to_mtl(blend_op);
  } else {
    desc.colorAttachments[0].blendingEnabled = NO;
  }
  desc.colorAttachments[0].writeMask = d3d_writemask_to_mtl(color_write_mask);

  if (textured) {
    s_geometry_textured_pso =
        [s_device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!s_geometry_textured_pso) {
      fprintf(stderr, "dx9mt_metal_viewer: geometry PSO failed: %s\n",
              error ? [[error localizedDescription] UTF8String] : "unknown");
      return;
    }
    s_geometry_textured_pso_stride = stride;
    s_geometry_textured_pso_key = draw_key;
  } else {
    s_geometry_pso =
        [s_device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!s_geometry_pso) {
      fprintf(stderr, "dx9mt_metal_viewer: geometry PSO failed: %s\n",
              error ? [[error localizedDescription] UTF8String] : "unknown");
      return;
    }
    s_geometry_pso_stride = stride;
    s_geometry_pso_key = draw_key;
  }

  fprintf(stderr,
          "dx9mt_metal_viewer: geometry PSO created stride=%u pos=%d col=%d "
          "uv=%d textured=%d blend=%u src=%u dst=%u\n",
          stride, has_position, has_color, has_texcoord, textured, blend_enable,
          eff_src_blend, eff_dst_blend);
}

static int init_metal(void) {
  NSError *error = nil;

  s_device = MTLCreateSystemDefaultDevice();
  if (!s_device) {
    fprintf(stderr, "dx9mt_metal_viewer: MTLCreateSystemDefaultDevice nil\n");
    return -1;
  }

  s_queue = [s_device newCommandQueue];
  if (!s_queue) {
    return -1;
  }

  s_library = [s_device newLibraryWithSource:s_shader_source
                                     options:nil
                                       error:&error];
  if (!s_library) {
    fprintf(stderr, "dx9mt_metal_viewer: shader compile failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown");
    return -1;
  }

  s_texture_cache = [[NSMutableDictionary alloc] init];
  s_texture_generation = [[NSMutableDictionary alloc] init];
  s_sampler_cache = [[NSMutableDictionary alloc] init];
  s_render_target_cache = [[NSMutableDictionary alloc] init];
  s_render_target_desc = [[NSMutableDictionary alloc] init];
  s_texture_rt_overrides = [[NSMutableDictionary alloc] init];
  s_vs_func_cache = [[NSMutableDictionary alloc] init];
  s_ps_func_cache = [[NSMutableDictionary alloc] init];
  s_translated_pso_cache = [[NSMutableDictionary alloc] init];
  s_depth_texture_cache = [[NSMutableDictionary alloc] init];
  s_depth_stencil_cache = [[NSMutableDictionary alloc] init];

  /* Create "no depth" state for overlay draws */
  {
    MTLDepthStencilDescriptor *no_depth_desc =
        [[MTLDepthStencilDescriptor alloc] init];
    no_depth_desc.depthCompareFunction = MTLCompareFunctionAlways;
    no_depth_desc.depthWriteEnabled = NO;
    s_no_depth_state =
        [s_device newDepthStencilStateWithDescriptor:no_depth_desc];
  }

  /* Overlay PSO (same as RB1) */
  {
    MTLRenderPipelineDescriptor *desc =
        [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = [s_library newFunctionWithName:@"overlay_vertex"];
    desc.fragmentFunction = [s_library newFunctionWithName:@"overlay_fragment"];
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    desc.colorAttachments[0].blendingEnabled = YES;
    desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    desc.colorAttachments[0].destinationRGBBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorZero;

    s_overlay_pso =
        [s_device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!s_overlay_pso) {
      fprintf(stderr, "dx9mt_metal_viewer: overlay PSO failed: %s\n",
              error ? [[error localizedDescription] UTF8String] : "unknown");
    }
  }

  fprintf(stderr, "dx9mt_metal_viewer: Metal init ok: %s overlay=%s\n",
          [[s_device name] UTF8String], s_overlay_pso ? "yes" : "no");
  return 0;
}

static void create_window(uint32_t width, uint32_t height) {
  if (s_window) {
    if (width == s_width && height == s_height) {
      return;
    }
    [s_window setContentSize:NSMakeSize(width, height)];
    s_metal_layer.drawableSize = CGSizeMake(width, height);
    s_width = width;
    s_height = height;
    [s_window
        setTitle:[NSString
                     stringWithFormat:@"dx9mt Metal [%ux%u]", width, height]];
    return;
  }

  NSRect frame = NSMakeRect(100, 100, width, height);
  NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                     NSWindowStyleMaskResizable;
  s_window = [[NSWindow alloc] initWithContentRect:frame
                                         styleMask:style
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
  [s_window
      setTitle:[NSString stringWithFormat:@"dx9mt Metal [%ux%u]", width,
                                         height]];
  [s_window setReleasedWhenClosed:NO];

  NSView *view = [[NSView alloc] initWithFrame:frame];
  [view setWantsLayer:YES];

  s_metal_layer = [CAMetalLayer layer];
  s_metal_layer.device = s_device;
  s_metal_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  s_metal_layer.framebufferOnly = YES;
  s_metal_layer.drawableSize = CGSizeMake(width, height);
  view.layer = s_metal_layer;

  [s_window setContentView:view];
  [s_window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];

  s_width = width;
  s_height = height;
}

static const char *d3d_usage_name(uint8_t usage) {
  switch (usage) {
  case 0:  return "POSITION";
  case 1:  return "BLENDWEIGHT";
  case 2:  return "BLENDINDICES";
  case 3:  return "NORMAL";
  case 4:  return "PSIZE";
  case 5:  return "TEXCOORD";
  case 6:  return "TANGENT";
  case 7:  return "BINORMAL";
  case 9:  return "POSITIONT";
  case 10: return "COLOR";
  case 11: return "FOG";
  case 12: return "DEPTH";
  default: return "?";
  }
}

static const char *d3d_decltype_name(uint8_t type) {
  switch (type) {
  case 0:  return "FLOAT1";
  case 1:  return "FLOAT2";
  case 2:  return "FLOAT3";
  case 3:  return "FLOAT4";
  case 4:  return "D3DCOLOR";
  case 5:  return "UBYTE4";
  case 6:  return "SHORT2";
  case 7:  return "SHORT4";
  case 8:  return "UBYTE4N";
  case 9:  return "SHORT2N";
  case 10: return "SHORT4N";
  case 11: return "USHORT2N";
  case 12: return "USHORT4N";
  case 15: return "FLOAT16_2";
  case 16: return "FLOAT16_4";
  default: return "?";
  }
}

static const char *d3d_texop_name(uint32_t op) {
  switch (op) {
  case 1:  return "DISABLE";
  case 2:  return "SELECTARG1";
  case 3:  return "SELECTARG2";
  case 4:  return "MODULATE";
  case 5:  return "MODULATE2X";
  case 6:  return "MODULATE4X";
  case 7:  return "ADD";
  case 8:  return "ADDSIGNED";
  case 12: return "BLENDDIFFUSEALPHA";
  case 13: return "BLENDTEXTUREALPHA";
  case 14: return "BLENDFACTORALPHA";
  default: return "?";
  }
}

static const char *d3d_texarg_name(uint32_t arg) {
  static char buf[64];
  const char *base;
  switch (arg & 0x0F) {
  case 0: base = "DIFFUSE"; break;
  case 1: base = "CURRENT"; break;
  case 2: base = "TEXTURE"; break;
  case 3: base = "TFACTOR"; break;
  case 4: base = "SPECULAR"; break;
  case 5: base = "TEMP"; break;
  case 6: base = "CONSTANT"; break;
  default: base = "?"; break;
  }
  if ((arg & 0x30) == 0) {
    return base;
  }
  snprintf(buf, sizeof(buf), "%s%s%s", base,
           (arg & 0x10) ? "|COMPLEMENT" : "",
           (arg & 0x20) ? "|ALPHAREPLICATE" : "");
  return buf;
}

static const char *d3d_fmt_name(uint32_t fmt) {
  switch (fmt) {
  case 21: return "A8R8G8B8";
  case 22: return "X8R8G8B8";
  case 28: return "A8";
  case 101: return "INDEX16";
  case 102: return "INDEX32";
  default:
    if (fmt == (uint32_t)D3DFMT_DXT1) return "DXT1";
    if (fmt == (uint32_t)D3DFMT_DXT3) return "DXT3";
    if (fmt == (uint32_t)D3DFMT_DXT5) return "DXT5";
    return "?";
  }
}

static const char *d3d_blend_name(uint32_t b) {
  switch (b) {
  case 1:  return "ZERO";
  case 2:  return "ONE";
  case 3:  return "SRCCOLOR";
  case 4:  return "INVSRCCOLOR";
  case 5:  return "SRCALPHA";
  case 6:  return "INVSRCALPHA";
  case 7:  return "DESTALPHA";
  case 8:  return "INVDESTALPHA";
  case 9:  return "DESTCOLOR";
  case 10: return "INVDESTCOLOR";
  case 11: return "SRCALPHASAT";
  default: return "?";
  }
}

/* ------------------------------------------------------------------ */
/* RB3 Phase 3: Shader translation + compilation                       */
/* ------------------------------------------------------------------ */

static id<MTLFunction> translate_and_compile_vs(
    const uint32_t *bytecode, uint32_t dword_count, uint32_t bc_hash) {
  NSNumber *key = @(bc_hash);

  /* Check cache */
  id cached = [s_vs_func_cache objectForKey:key];
  if (cached) return (cached == (id)[NSNull null]) ? nil : cached;

  /* Parse */
  dx9mt_sm_program prog;
  if (dx9mt_sm_parse(bytecode, dword_count, &prog) != 0) {
    fprintf(stderr, "dx9mt: VS 0x%08x parse failed: %s\n",
            bc_hash, prog.error_msg);
    [s_vs_func_cache setObject:[NSNull null] forKey:key];
    return nil;
  }

  /* Emit MSL */
  dx9mt_msl_emit_result msl;
  if (dx9mt_msl_emit_vs(&prog, bc_hash, &msl) != 0) {
    fprintf(stderr, "dx9mt: VS 0x%08x emit failed: %s\n",
            bc_hash, msl.error_msg);
    [s_vs_func_cache setObject:[NSNull null] forKey:key];
    return nil;
  }

  /* Compile */
  NSString *src = [NSString stringWithUTF8String:msl.source];
  NSError *err = nil;
  id<MTLLibrary> lib = [s_device newLibraryWithSource:src options:nil error:&err];
  if (!lib) {
    fprintf(stderr, "dx9mt: VS 0x%08x compile failed: %s\n",
            bc_hash, [[err localizedDescription] UTF8String]);
    fprintf(stderr, "--- VS MSL source ---\n%s\n--- end ---\n", msl.source);
    dx9mt_sm_dump(&prog, stderr);
    [s_vs_func_cache setObject:[NSNull null] forKey:key];
    return nil;
  }

  NSString *entry = [NSString stringWithUTF8String:msl.entry_name];
  id<MTLFunction> func = [lib newFunctionWithName:entry];
  if (!func) {
    fprintf(stderr, "dx9mt: VS 0x%08x entry '%s' not found\n",
            bc_hash, msl.entry_name);
    [s_vs_func_cache setObject:[NSNull null] forKey:key];
    return nil;
  }

  fprintf(stderr, "dx9mt: VS 0x%08x compiled OK (%u instructions)\n",
          bc_hash, prog.instruction_count);
  [s_vs_func_cache setObject:func forKey:key];
  return func;
}

static id<MTLFunction> translate_and_compile_ps(
    const uint32_t *bytecode, uint32_t dword_count, uint32_t bc_hash) {
  NSNumber *key = @(bc_hash);

  id cached = [s_ps_func_cache objectForKey:key];
  if (cached) return (cached == (id)[NSNull null]) ? nil : cached;

  dx9mt_sm_program prog;
  if (dx9mt_sm_parse(bytecode, dword_count, &prog) != 0) {
    fprintf(stderr, "dx9mt: PS 0x%08x parse failed: %s\n",
            bc_hash, prog.error_msg);
    [s_ps_func_cache setObject:[NSNull null] forKey:key];
    return nil;
  }

  dx9mt_msl_emit_result msl;
  if (dx9mt_msl_emit_ps(&prog, bc_hash, &msl) != 0) {
    fprintf(stderr, "dx9mt: PS 0x%08x emit failed: %s\n",
            bc_hash, msl.error_msg);
    [s_ps_func_cache setObject:[NSNull null] forKey:key];
    return nil;
  }

  NSString *src = [NSString stringWithUTF8String:msl.source];
  NSError *err = nil;
  id<MTLLibrary> lib = [s_device newLibraryWithSource:src options:nil error:&err];
  if (!lib) {
    fprintf(stderr, "dx9mt: PS 0x%08x compile failed: %s\n",
            bc_hash, [[err localizedDescription] UTF8String]);
    fprintf(stderr, "--- PS MSL source ---\n%s\n--- end ---\n", msl.source);
    dx9mt_sm_dump(&prog, stderr);
    [s_ps_func_cache setObject:[NSNull null] forKey:key];
    return nil;
  }

  NSString *entry = [NSString stringWithUTF8String:msl.entry_name];
  id<MTLFunction> func = [lib newFunctionWithName:entry];
  if (!func) {
    fprintf(stderr, "dx9mt: PS 0x%08x entry '%s' not found\n",
            bc_hash, msl.entry_name);
    [s_ps_func_cache setObject:[NSNull null] forKey:key];
    return nil;
  }

  fprintf(stderr, "dx9mt: PS 0x%08x compiled OK (%u instructions)\n",
          bc_hash, prog.instruction_count);
  [s_ps_func_cache setObject:func forKey:key];
  return func;
}

static id<MTLRenderPipelineState> create_translated_pso(
    id<MTLFunction> vs_func, id<MTLFunction> ps_func,
    const dx9mt_d3d_vertex_element *elems, uint16_t elem_count,
    uint32_t stride, int textured,
    uint32_t blend_enable, uint32_t src_blend, uint32_t dst_blend,
    uint32_t blend_op, uint32_t color_write_mask,
    uint64_t pso_key) {
  NSNumber *key = @(pso_key);
  id cached = [s_translated_pso_cache objectForKey:key];
  if (cached) return (cached == (id)[NSNull null]) ? nil : cached;

  MTLVertexDescriptor *vd = [[MTLVertexDescriptor alloc] init];
  /* Map vertex elements using attribute index = register number based on usage */
  for (uint16_t e = 0; e < elem_count; ++e) {
    MTLVertexFormat fmt = decl_type_to_mtl(elems[e].type);
    if (fmt == MTLVertexFormatInvalid) continue;
    /* For translated shaders: attribute index matches v# register.
     * POSITION -> v0, COLOR -> v1, TEXCOORD0 -> v2, etc.
     * This must match the VS_In struct attribute indices. */
    uint32_t attr_idx;
    if (elems[e].usage == D3DDECLUSAGE_POSITION || elems[e].usage == D3DDECLUSAGE_POSITIONT) {
      attr_idx = 0;
    } else if (elems[e].usage == D3DDECLUSAGE_COLOR && elems[e].usage_index == 0) {
      attr_idx = 1;
    } else if (elems[e].usage == D3DDECLUSAGE_TEXCOORD && elems[e].usage_index == 0) {
      attr_idx = 2;
    } else if (elems[e].usage == D3DDECLUSAGE_NORMAL && elems[e].usage_index == 0) {
      attr_idx = 3;
    } else if (elems[e].usage == D3DDECLUSAGE_TEXCOORD && elems[e].usage_index == 1) {
      attr_idx = 4;
    } else if (elems[e].usage == D3DDECLUSAGE_COLOR && elems[e].usage_index == 1) {
      attr_idx = 5;
    } else if (elems[e].usage == 1 /* BLENDWEIGHT */ && elems[e].usage_index == 0) {
      attr_idx = 6;
    } else if (elems[e].usage == 2 /* BLENDINDICES */ && elems[e].usage_index == 0) {
      attr_idx = 7;
    } else {
      continue; /* Skip unmapped semantics */
    }
    vd.attributes[attr_idx].format = fmt;
    vd.attributes[attr_idx].offset = elems[e].offset;
    vd.attributes[attr_idx].bufferIndex = 0;
  }
  vd.layouts[0].stride = stride;
  vd.layouts[0].stepRate = 1;
  vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

  MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction = vs_func;
  desc.fragmentFunction = ps_func;
  desc.vertexDescriptor = vd;
  desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

  if (blend_enable) {
    desc.colorAttachments[0].blendingEnabled = YES;
    desc.colorAttachments[0].sourceRGBBlendFactor = d3d_blend_to_mtl(src_blend, 1);
    desc.colorAttachments[0].destinationRGBBlendFactor = d3d_blend_to_mtl(dst_blend, 0);
    desc.colorAttachments[0].rgbBlendOperation = d3d_blendop_to_mtl(blend_op);
    desc.colorAttachments[0].sourceAlphaBlendFactor = d3d_blend_to_mtl(src_blend, 1);
    desc.colorAttachments[0].destinationAlphaBlendFactor = d3d_blend_to_mtl(dst_blend, 0);
    desc.colorAttachments[0].alphaBlendOperation = d3d_blendop_to_mtl(blend_op);
  }
  desc.colorAttachments[0].writeMask = d3d_writemask_to_mtl(color_write_mask);

  NSError *err = nil;
  id<MTLRenderPipelineState> pso =
      [s_device newRenderPipelineStateWithDescriptor:desc error:&err];
  if (!pso) {
    fprintf(stderr, "dx9mt: translated PSO creation failed: %s\n",
            [[err localizedDescription] UTF8String]);
    [s_translated_pso_cache setObject:[NSNull null] forKey:key];
    return nil;
  }

  [s_translated_pso_cache setObject:pso forKey:key];
  return pso;
}

static void dump_frame_to(const volatile unsigned char *ipc_base,
                          const char *path) {
  const volatile dx9mt_metal_ipc_header *hdr =
      (const volatile dx9mt_metal_ipc_header *)ipc_base;
  const volatile dx9mt_metal_ipc_draw *draws =
      (const volatile dx9mt_metal_ipc_draw *)(ipc_base +
                                              sizeof(dx9mt_metal_ipc_header));
  uint32_t bulk_off = hdr->bulk_data_offset;
  uint32_t draw_count = hdr->draw_count;

  ensure_output_dir();

  FILE *f = fopen(path, "w");
  if (!f) {
    fprintf(stderr, "dx9mt_metal_viewer: cannot open dump file %s\n", path);
    return;
  }

  fprintf(f, "=== FRAME DUMP ===\n");
  fprintf(f, "resolution: %ux%u  draws: %u  frame_id: %u\n",
          hdr->width, hdr->height, draw_count, hdr->frame_id);
  fprintf(f, "clear: %s  color: 0x%08x  present_rt: %u\n",
          hdr->have_clear ? "yes" : "no", hdr->clear_color_argb,
          hdr->present_render_target_id);
  fprintf(f, "\n");

  for (uint32_t i = 0; i < draw_count && i < DX9MT_METAL_IPC_MAX_DRAWS; ++i) {
    const volatile dx9mt_metal_ipc_draw *d = &draws[i];

    fprintf(f, "--- draw[%u] ---\n", i);
    fprintf(f, "  prim_type=%u  prim_count=%u  base_vertex=%d  start_index=%u\n",
            d->primitive_type, d->primitive_count, d->base_vertex,
            d->start_index);
    fprintf(f, "  stride=%u  fvf=0x%08x  vs_id=%u  ps_id=%u\n",
            d->stream0_stride, d->fvf, d->vertex_shader_id, d->pixel_shader_id);
    fprintf(f, "  rt_id=%u  rt_tex_id=%u  rt_size=%ux%u  rt_fmt=%s\n",
            d->render_target_id, d->render_target_texture_id,
            d->render_target_width, d->render_target_height,
            d3d_fmt_name(d->render_target_format));
    fprintf(f, "  viewport=(%u,%u %ux%u) z=[%.3f,%.3f]\n",
            d->viewport_x, d->viewport_y, d->viewport_width,
            d->viewport_height, d->viewport_min_z, d->viewport_max_z);

    /* Vertex declaration */
    fprintf(f, "  decl_count=%u", d->decl_count);
    if (d->decl_count > 0 && d->decl_count < 64) {
      const dx9mt_d3d_vertex_element *elems =
          (const dx9mt_d3d_vertex_element *)(ipc_base + bulk_off +
                                             d->decl_bulk_offset);
      fprintf(f, ":\n");
      for (uint16_t e = 0; e < d->decl_count; ++e) {
        fprintf(f, "    [%u] stream=%u offset=%u type=%s usage=%s idx=%u\n",
                e, elems[e].stream, elems[e].offset,
                d3d_decltype_name(elems[e].type),
                d3d_usage_name(elems[e].usage), elems[e].usage_index);
      }
    } else {
      fprintf(f, "\n");
    }

    /* Textures and samplers */
    for (uint32_t s = 0; s < DX9MT_MAX_PS_SAMPLERS; ++s) {
      if (d->tex_id[s] == 0) continue;
      fprintf(f, "  tex%u: id=%u gen=%u fmt=%s size=%ux%u pitch=%u upload=%u\n",
              s, d->tex_id[s], d->tex_generation[s],
              d3d_fmt_name(d->tex_format[s]),
              d->tex_width[s], d->tex_height[s],
              d->tex_pitch[s], d->tex_bulk_size[s]);
      fprintf(f, "  sampler%u: min=%u mag=%u mip=%u addr=(%u,%u,%u)\n",
              s, d->sampler_min_filter[s], d->sampler_mag_filter[s],
              d->sampler_mip_filter[s], d->sampler_address_u[s],
              d->sampler_address_v[s], d->sampler_address_w[s]);
    }

    /* TSS combiner */
    fprintf(f, "  tss0: color_op=%s  arg1=%s  arg2=%s\n",
            d3d_texop_name(d->tss0_color_op),
            d3d_texarg_name(d->tss0_color_arg1),
            d3d_texarg_name(d->tss0_color_arg2));
    fprintf(f, "  tss0: alpha_op=%s  arg1=%s  arg2=%s\n",
            d3d_texop_name(d->tss0_alpha_op),
            d3d_texarg_name(d->tss0_alpha_arg1),
            d3d_texarg_name(d->tss0_alpha_arg2));
    fprintf(f, "  texture_factor=0x%08x\n", d->rs_texture_factor);

    /* Blend / alpha test */
    fprintf(f, "  blend: enable=%u  src=%s  dst=%s\n",
            d->rs_alpha_blend_enable,
            d3d_blend_name(d->rs_src_blend),
            d3d_blend_name(d->rs_dest_blend));
    fprintf(f, "  alpha_test: enable=%u  ref=%u  func=%u\n",
            d->rs_alpha_test_enable, d->rs_alpha_ref, d->rs_alpha_func);
    fprintf(f, "  depth: enable=%u  write=%u  func=%u  cull=%u\n",
            d->rs_zenable, d->rs_zwriteenable, d->rs_zfunc, d->rs_cull_mode);
    fprintf(f, "  stencil: enable=%u  func=%u  ref=%u  mask=0x%08x  writemask=0x%08x\n",
            d->rs_stencilenable, d->rs_stencilfunc, d->rs_stencilref,
            d->rs_stencilmask, d->rs_stencilwritemask);

    /* Buffer sizes */
    fprintf(f, "  vb: offset=%u size=%u  ib: offset=%u size=%u\n",
            d->vb_bulk_offset, d->vb_bulk_size,
            d->ib_bulk_offset, d->ib_bulk_size);
    fprintf(f, "  vs_const: offset=%u size=%u  ps_const: offset=%u size=%u\n",
            d->vs_constants_bulk_offset, d->vs_constants_size,
            d->ps_constants_bulk_offset, d->ps_constants_size);

    /* Shader bytecode */
    fprintf(f, "  vs_id=%u  vs_bc: offset=%u size=%u  ps_bc: offset=%u size=%u\n",
            d->vertex_shader_id,
            d->vs_bytecode_bulk_offset, d->vs_bytecode_bulk_size,
            d->ps_bytecode_bulk_offset, d->ps_bytecode_bulk_size);
    if (d->vs_bytecode_bulk_size >= 4) {
      const uint32_t *bc = (const uint32_t *)(ipc_base + bulk_off +
                                               d->vs_bytecode_bulk_offset);
      uint32_t dw_count = d->vs_bytecode_bulk_size / 4;
      fprintf(f, "    vs_bc[0..3]: %08x %08x %08x %08x (%u dwords, ver=%u.%u)\n",
              bc[0], dw_count > 1 ? bc[1] : 0,
              dw_count > 2 ? bc[2] : 0, dw_count > 3 ? bc[3] : 0,
              dw_count, (bc[0] >> 8) & 0xFF, bc[0] & 0xFF);
    }
    if (d->ps_bytecode_bulk_size >= 4) {
      const uint32_t *bc = (const uint32_t *)(ipc_base + bulk_off +
                                               d->ps_bytecode_bulk_offset);
      uint32_t dw_count = d->ps_bytecode_bulk_size / 4;
      fprintf(f, "    ps_bc[0..3]: %08x %08x %08x %08x (%u dwords, ver=%u.%u)\n",
              bc[0], dw_count > 1 ? bc[1] : 0,
              dw_count > 2 ? bc[2] : 0, dw_count > 3 ? bc[3] : 0,
              dw_count, (bc[0] >> 8) & 0xFF, bc[0] & 0xFF);
    }

    /* Dump first few vertices (raw hex + interpreted color if COLOR present) */
    if (d->vb_bulk_size > 0 && d->stream0_stride > 0) {
      const uint8_t *vb = (const uint8_t *)(ipc_base + bulk_off +
                                             d->vb_bulk_offset);
      uint32_t stride = d->stream0_stride;
      uint32_t vert_count = d->vb_bulk_size / stride;
      if (vert_count > 4) vert_count = 4;

      /* Find color offset from decl */
      int color_offset = -1;
      int pos_offset = -1;
      int uv_offset = -1;
      uint8_t pos_type = 0;
      if (d->decl_count > 0 && d->decl_count < 64) {
        const dx9mt_d3d_vertex_element *elems =
            (const dx9mt_d3d_vertex_element *)(ipc_base + bulk_off +
                                               d->decl_bulk_offset);
        for (uint16_t e = 0; e < d->decl_count; ++e) {
          if (elems[e].usage == D3DDECLUSAGE_COLOR && elems[e].usage_index == 0)
            color_offset = elems[e].offset;
          if ((elems[e].usage == D3DDECLUSAGE_POSITION ||
               elems[e].usage == D3DDECLUSAGE_POSITIONT) &&
              elems[e].usage_index == 0) {
            pos_offset = elems[e].offset;
            pos_type = elems[e].type;
          }
          if (elems[e].usage == D3DDECLUSAGE_TEXCOORD && elems[e].usage_index == 0)
            uv_offset = elems[e].offset;
        }
      }

      for (uint32_t v = 0; v < vert_count; ++v) {
        const uint8_t *vert = vb + (d->stream0_offset + v * stride);
        fprintf(f, "  vert[%u]:", v);
        if (pos_offset >= 0) {
          const float *p = (const float *)(vert + pos_offset);
          if (pos_type == D3DDECLTYPE_FLOAT4)
            fprintf(f, " pos=(%.2f,%.2f,%.2f,%.2f)", p[0], p[1], p[2], p[3]);
          else
            fprintf(f, " pos=(%.2f,%.2f,%.2f)", p[0], p[1], p[2]);
        }
        if (color_offset >= 0) {
          uint32_t c;
          memcpy(&c, vert + color_offset, 4);
          fprintf(f, " color=0x%08x(A=%u R=%u G=%u B=%u)",
                  c, (c >> 24) & 0xFF, (c >> 16) & 0xFF,
                  (c >> 8) & 0xFF, c & 0xFF);
        }
        if (uv_offset >= 0) {
          const float *uv = (const float *)(vert + uv_offset);
          fprintf(f, " uv=(%.4f,%.4f)", uv[0], uv[1]);
        }
        fprintf(f, "\n");
      }
    }

    /* Save texture data to file */
    for (uint32_t s = 0; s < DX9MT_MAX_PS_SAMPLERS; ++s) {
      if (d->tex_bulk_size[s] > 0 && d->tex_id[s] != 0) {
        char tex_name[64];
        char tex_path[PATH_MAX];
        snprintf(tex_name, sizeof(tex_name), "dx9mt_tex_%u_s%u.raw",
                 d->tex_id[s], s);
        build_output_path(tex_path, sizeof(tex_path), tex_name);
        FILE *tf = fopen(tex_path, "wb");
        if (tf) {
          const void *tex_data = (const void *)(ipc_base + bulk_off +
                                                 d->tex_bulk_offset[s]);
          fwrite(tex_data, 1, d->tex_bulk_size[s], tf);
          fclose(tf);
          fprintf(f, "  >> texture saved: %s (%u bytes, %s %ux%u)\n",
                  tex_path, d->tex_bulk_size[s],
                  d3d_fmt_name(d->tex_format[s]),
                  d->tex_width[s], d->tex_height[s]);
        }
      }
    }
    fprintf(f, "\n");
  }

  fclose(f);
  fprintf(stderr, "dx9mt_metal_viewer: frame dump written to %s (%u draws)\n",
          path, draw_count);
}

static void dump_frame(const volatile unsigned char *ipc_base) {
  char path[PATH_MAX];
  build_output_path(path, sizeof(path), "dx9mt_frame_dump.txt");
  dump_frame_to(ipc_base, path);
}

static void render_frame(const volatile unsigned char *ipc_base) {
  const volatile dx9mt_metal_ipc_header *hdr =
      (const volatile dx9mt_metal_ipc_header *)ipc_base;
  const volatile dx9mt_metal_ipc_draw *draws =
      (const volatile dx9mt_metal_ipc_draw *)(ipc_base +
                                              sizeof(dx9mt_metal_ipc_header));
  uint32_t bulk_off = hdr->bulk_data_offset;
  uint32_t draw_count = hdr->draw_count;

  @autoreleasepool {
    id<CAMetalDrawable> drawable = [s_metal_layer nextDrawable];
    if (!drawable) {
      return;
    }

    id<MTLCommandBuffer> cmd_buf = [s_queue commandBuffer];
    if (!cmd_buf) {
      return;
    }

    /* Clear color from the game's Clear() call */
    float r, g, b, a;
    if (hdr->have_clear) {
      a = ((hdr->clear_color_argb >> 24) & 0xFFu) / 255.0f;
      r = ((hdr->clear_color_argb >> 16) & 0xFFu) / 255.0f;
      g = ((hdr->clear_color_argb >> 8) & 0xFFu) / 255.0f;
      b = (hdr->clear_color_argb & 0xFFu) / 255.0f;
    } else {
      r = g = b = 0.0f;
      a = 1.0f;
    }

    /*
     * Step 1 pass routing: replay draws into their bound render target ID.
     * Prefer the present-time render target hint from the frontend, and
     * fall back to inferring from the last non-zero draw RT when missing.
     */
    uint32_t primary_rt_id = hdr->present_render_target_id;
    if (primary_rt_id == 0) {
      for (int32_t i = (int32_t)draw_count - 1; i >= 0; --i) {
        if (draws[i].render_target_id != 0) {
          primary_rt_id = draws[i].render_target_id;
          break;
        }
      }
    }

    id<MTLRenderCommandEncoder> encoder = nil;
    uint32_t active_rt_id = UINT32_MAX;
    int active_target_is_drawable = 0;
    NSMutableSet *cleared_targets = [[NSMutableSet alloc] init];
    NSNumber *drawable_key = @(0u);

    /* RB4: depth clear value from Clear() call */
    float clear_depth = 1.0f;
    int clear_depth_flag = 0;
    if (hdr->have_clear) {
      clear_depth = hdr->clear_z;
      clear_depth_flag = (hdr->clear_flags & 0x2) ? 1 : 0;
    }

    /* RB3: Render actual geometry from per-draw IPC data */
    for (uint32_t i = 0; i < draw_count && i < DX9MT_METAL_IPC_MAX_DRAWS;
         ++i) {
      const volatile dx9mt_metal_ipc_draw *d = &draws[i];
      uint32_t draw_rt_id;
      int target_is_drawable;
      id<MTLTexture> draw_target_texture = nil;
      uint32_t stride = d->stream0_stride;
      uint32_t index_count;
      MTLIndexType mtl_index_type;
      const dx9mt_d3d_vertex_element *elems = NULL;
      id<MTLTexture> draw_texture = nil;
      id<MTLSamplerState> draw_sampler = nil;
      id<MTLRenderPipelineState> geometry_pso = nil;
      int decl_has_color = 0;
      int decl_has_texcoord = 0;
      int decl_has_positiont = 0;
      int expects_texture = 0;
      int textured = 0;
      dx9mt_d3d_vertex_element fvf_elems[16];
      uint16_t fvf_elem_count = 0;

      if (d->vb_bulk_size == 0 || d->ib_bulk_size == 0 || stride == 0) {
        continue;
      }
      if (d->primitive_count == 0) {
        continue;
      }

      draw_rt_id = d->render_target_id;
      target_is_drawable =
          (draw_rt_id == 0 || (primary_rt_id != 0 && draw_rt_id == primary_rt_id));
      if (target_is_drawable) {
        draw_rt_id = 0;
        draw_target_texture = drawable.texture;
      } else {
        draw_target_texture = render_target_texture_for_draw(d);
      }
      if (!draw_target_texture) {
        continue;
      }

      if (!encoder || draw_rt_id != active_rt_id ||
          target_is_drawable != active_target_is_drawable) {
        MTLRenderPassDescriptor *pass_desc;
        NSNumber *target_key;
        BOOL seen_target;
        id<MTLTexture> depth_tex = nil;

        if (encoder) {
          [encoder endEncoding];
        }

        pass_desc = [MTLRenderPassDescriptor renderPassDescriptor];
        pass_desc.colorAttachments[0].texture = draw_target_texture;
        pass_desc.colorAttachments[0].storeAction = MTLStoreActionStore;
        target_key = target_is_drawable ? drawable_key : @(draw_rt_id);
        seen_target = [cleared_targets containsObject:target_key];
        if (seen_target) {
          pass_desc.colorAttachments[0].loadAction = MTLLoadActionLoad;
        } else {
          pass_desc.colorAttachments[0].loadAction = MTLLoadActionClear;
          if (target_is_drawable) {
            pass_desc.colorAttachments[0].clearColor = MTLClearColorMake(r, g, b, a);
          } else {
            pass_desc.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
          }
          [cleared_targets addObject:target_key];
        }

        /* RB4: attach depth texture */
        if (target_is_drawable) {
          depth_tex = ensure_drawable_depth_texture(s_width, s_height);
        } else {
          depth_tex = depth_texture_for_rt(
              draw_rt_id, d->render_target_width, d->render_target_height);
        }
        if (depth_tex) {
          pass_desc.depthAttachment.texture = depth_tex;
          pass_desc.depthAttachment.storeAction = MTLStoreActionStore;
          if (seen_target) {
            pass_desc.depthAttachment.loadAction = MTLLoadActionLoad;
          } else {
            if (target_is_drawable && clear_depth_flag) {
              pass_desc.depthAttachment.loadAction = MTLLoadActionClear;
              pass_desc.depthAttachment.clearDepth = clear_depth;
            } else {
              pass_desc.depthAttachment.loadAction = MTLLoadActionClear;
              pass_desc.depthAttachment.clearDepth = 1.0;
            }
          }
        }

        encoder = [cmd_buf renderCommandEncoderWithDescriptor:pass_desc];
        if (!encoder) {
          continue;
        }
        active_rt_id = draw_rt_id;
        active_target_is_drawable = target_is_drawable;
      }

      draw_texture = texture_for_draw_stage(ipc_base, bulk_off, d, 0);

      /* Parse vertex declaration from bulk data to build PSO.
       * Fall back to FVF-derived elements when no declaration is present. */
      if (d->decl_count > 0 && d->decl_count < 64) {
        elems = (const dx9mt_d3d_vertex_element *)(ipc_base + bulk_off +
                                                   d->decl_bulk_offset);
        scan_decl_semantics(elems, d->decl_count, NULL, &decl_has_color,
                            &decl_has_texcoord, &decl_has_positiont);
      } else if (d->fvf != 0) {
        fvf_elem_count = fvf_to_elements(d->fvf, fvf_elems, 16);
        if (fvf_elem_count > 0) {
          elems = fvf_elems;
          scan_decl_semantics(elems, fvf_elem_count, NULL, &decl_has_color,
                              &decl_has_texcoord, &decl_has_positiont);
        }
      }

      expects_texture = (d->tex_id[0] != 0 && decl_has_texcoord);
      if (expects_texture && !draw_texture) {
        /*
         * Texture content is unavailable (typically render-to-texture path not
         * captured yet). Skip instead of drawing a solid white fallback quad.
         */
        continue;
      }

      if (draw_texture && decl_has_texcoord) {
        draw_sampler = sampler_state_for_draw_stage(d, 0);
        textured = 1;
      } else {
        draw_texture = nil;
      }

      if (elems) {
        uint16_t eff_count = (fvf_elem_count > 0) ? fvf_elem_count
                                                   : d->decl_count;
        ensure_geometry_pso(stride, elems, eff_count, textured,
                            d->rs_alpha_blend_enable, d->rs_src_blend,
                            d->rs_dest_blend, d->rs_blendop,
                            d->rs_colorwriteenable);
      }

      geometry_pso = textured ? s_geometry_textured_pso : s_geometry_pso;
      if (!geometry_pso) {
        continue;
      }

      /* RB3 Phase 3: shader translation (mandatory when bytecode present) */
      int use_translated = 0;
      int has_shader_bytecode =
          (d->vs_bytecode_bulk_size >= 8 && d->ps_bytecode_bulk_size >= 8 &&
           !decl_has_positiont);

      if (has_shader_bytecode) {
        const uint32_t *vs_bc = (const uint32_t *)(ipc_base + bulk_off +
                                                     d->vs_bytecode_bulk_offset);
        const uint32_t *ps_bc = (const uint32_t *)(ipc_base + bulk_off +
                                                     d->ps_bytecode_bulk_offset);
        uint32_t vs_dwords = d->vs_bytecode_bulk_size / 4;
        uint32_t ps_dwords = d->ps_bytecode_bulk_size / 4;

        /* Validate version tokens: VS=0xFFFExxxx, PS=0xFFFFxxxx */
        uint32_t vs_ver = vs_bc[0] & 0xFFFF0000u;
        uint32_t ps_ver = ps_bc[0] & 0xFFFF0000u;
        if (vs_ver != 0xFFFE0000u || ps_ver != 0xFFFF0000u) {
          continue; /* invalid bytecode — skip draw */
        }

        uint32_t vs_hash = dx9mt_sm_bytecode_hash(vs_bc, vs_dwords);
        uint32_t ps_hash = dx9mt_sm_bytecode_hash(ps_bc, ps_dwords);

        id<MTLFunction> vs_func = translate_and_compile_vs(vs_bc, vs_dwords, vs_hash);
        id<MTLFunction> ps_func = translate_and_compile_ps(ps_bc, ps_dwords, ps_hash);

        if (vs_func && ps_func && elems) {
          uint16_t eff_count = (fvf_elem_count > 0) ? fvf_elem_count
                                                     : d->decl_count;
          uint64_t pso_key = ((uint64_t)vs_hash << 32) | ps_hash;
          pso_key ^= (uint64_t)stride * 0x9E3779B97F4A7C15ULL;
          pso_key ^= ((uint64_t)d->rs_alpha_blend_enable << 48) |
                     ((uint64_t)d->rs_src_blend << 40) |
                     ((uint64_t)d->rs_dest_blend << 32);
          pso_key ^= ((uint64_t)d->rs_blendop << 24) |
                     ((uint64_t)d->rs_colorwriteenable << 16);

          id<MTLRenderPipelineState> translated_pso = create_translated_pso(
              vs_func, ps_func, elems, eff_count, stride, textured,
              d->rs_alpha_blend_enable, d->rs_src_blend,
              d->rs_dest_blend, d->rs_blendop,
              d->rs_colorwriteenable, pso_key);
          if (translated_pso) {
            geometry_pso = translated_pso;
            use_translated = 1;
          }
        }
      }

      /* Create Metal buffers from IPC bulk data */
      const void *vb_data = (const void *)(ipc_base + bulk_off + d->vb_bulk_offset);
      const void *ib_data = (const void *)(ipc_base + bulk_off + d->ib_bulk_offset);

      id<MTLBuffer> vb_buf =
          [s_device newBufferWithBytes:vb_data
                               length:d->vb_bulk_size
                              options:MTLResourceStorageModeShared];
      if (!vb_buf) {
        continue;
      }

      id<MTLBuffer> ib_buf =
          [s_device newBufferWithBytes:ib_data
                               length:d->ib_bulk_size
                              options:MTLResourceStorageModeShared];
      if (!ib_buf) {
        continue;
      }

      /* Set viewport from draw entry */
      if (d->viewport_width > 0 && d->viewport_height > 0) {
        MTLViewport vp;
        vp.originX = d->viewport_x;
        vp.originY = d->viewport_y;
        vp.width = d->viewport_width;
        vp.height = d->viewport_height;
        vp.znear = d->viewport_min_z;
        vp.zfar = d->viewport_max_z;
        [encoder setViewport:vp];
      }

      /* Scissor rect */
      if (d->rs_scissortestenable &&
          d->scissor_right > d->scissor_left &&
          d->scissor_bottom > d->scissor_top) {
        uint32_t rt_w = target_is_drawable ? s_width : d->render_target_width;
        uint32_t rt_h = target_is_drawable ? s_height : d->render_target_height;
        MTLScissorRect sr;
        sr.x = (NSUInteger)d->scissor_left;
        sr.y = (NSUInteger)d->scissor_top;
        sr.width = (NSUInteger)(d->scissor_right - d->scissor_left);
        sr.height = (NSUInteger)(d->scissor_bottom - d->scissor_top);
        if (sr.x + sr.width > rt_w) sr.width = rt_w - sr.x;
        if (sr.y + sr.height > rt_h) sr.height = rt_h - sr.y;
        [encoder setScissorRect:sr];
      }

      /* Index type */
      if (d->index_format == D3DFMT_INDEX32) {
        mtl_index_type = MTLIndexTypeUInt32;
      } else {
        mtl_index_type = MTLIndexTypeUInt16;
      }

      index_count = d3d_index_count(d->primitive_type, d->primitive_count);

      [encoder setRenderPipelineState:geometry_pso];
      [encoder setVertexBuffer:vb_buf offset:d->stream0_offset atIndex:0];

      if (use_translated) {
        /* Translated shader path: bind full constant arrays */
        if (d->vs_constants_size > 0) {
          const void *vs_data =
              (const void *)(ipc_base + bulk_off + d->vs_constants_bulk_offset);
          [encoder setVertexBytes:vs_data length:d->vs_constants_size atIndex:1];
        } else {
          static const float zero[4] = {0, 0, 0, 0};
          [encoder setVertexBytes:zero length:sizeof(zero) atIndex:1];
        }
        if (d->ps_constants_size > 0) {
          const void *ps_data =
              (const void *)(ipc_base + bulk_off + d->ps_constants_bulk_offset);
          [encoder setFragmentBytes:ps_data length:d->ps_constants_size atIndex:0];
        } else {
          static const float zero[4] = {0, 0, 0, 0};
          [encoder setFragmentBytes:zero length:sizeof(zero) atIndex:0];
        }
        /* Bind textures and samplers for all active stages */
        for (uint32_t s = 0; s < DX9MT_MAX_PS_SAMPLERS; ++s) {
          if (d->tex_id[s] == 0) continue;
          id<MTLTexture> stage_tex =
              texture_for_draw_stage(ipc_base, bulk_off, d, s);
          if (stage_tex) {
            [encoder setFragmentTexture:stage_tex atIndex:s];
            [encoder setFragmentSamplerState:sampler_state_for_draw_stage(d, s)
                                     atIndex:s];
          }
        }
      } else {
        /* Hardcoded shader path (TSS combiner / ps_c0 tint / passthrough) */
        uint32_t alpha_func = d->rs_alpha_func;
        uint32_t color_op = d->tss0_color_op;
        uint32_t alpha_op = d->tss0_alpha_op;
        if (alpha_func < D3DCMP_NEVER || alpha_func > D3DCMP_ALWAYS) {
          alpha_func = D3DCMP_ALWAYS;
        }
        if (color_op < D3DTOP_DISABLE || color_op > D3DTOP_PREMODULATE) {
          color_op = D3DTOP_MODULATE;
        }
        if (alpha_op < D3DTOP_DISABLE || alpha_op > D3DTOP_PREMODULATE) {
          alpha_op = D3DTOP_SELECTARG1;
        }
        struct {
          uint32_t use_vertex_color;
          uint32_t use_stage0_combiner;
          uint32_t alpha_only;
          uint32_t force_alpha_one;
          uint32_t alpha_test_enable;
          float alpha_ref;
          uint32_t alpha_func;
          uint32_t color_op;
          uint32_t color_arg1;
          uint32_t color_arg2;
          uint32_t alpha_op;
          uint32_t alpha_arg1;
          uint32_t alpha_arg2;
          uint32_t texture_factor_argb;
          uint32_t has_pixel_shader;
          uint32_t fog_enable;
          float ps_c0[4];
          uint32_t fog_mode;
          float fog_start;
          float fog_end;
          float fog_density;
          float fog_color[4];
        } frag_params;
        memset(&frag_params, 0, sizeof(frag_params));
        frag_params.use_vertex_color = (uint32_t)(decl_has_color ? 1 : 0);
        frag_params.use_stage0_combiner =
            (uint32_t)(d->pixel_shader_id == 0 ? 1 : 0);
        frag_params.alpha_only =
            (uint32_t)(d->tex_format[0] == D3DFMT_A8 ? 1 : 0);
        frag_params.force_alpha_one =
            (uint32_t)(d->tex_format[0] == D3DFMT_X8R8G8B8 ? 1 : 0);
        frag_params.alpha_test_enable =
            (uint32_t)(d->rs_alpha_test_enable ? 1 : 0);
        frag_params.alpha_ref = (float)(d->rs_alpha_ref & 0xFFu) / 255.0f;
        frag_params.alpha_func = alpha_func;
        frag_params.color_op = color_op;
        frag_params.color_arg1 = d->tss0_color_arg1;
        frag_params.color_arg2 = d->tss0_color_arg2;
        frag_params.alpha_op = alpha_op;
        frag_params.alpha_arg1 = d->tss0_alpha_arg1;
        frag_params.alpha_arg2 = d->tss0_alpha_arg2;
        frag_params.texture_factor_argb = d->rs_texture_factor;
        frag_params.has_pixel_shader =
            (uint32_t)(d->pixel_shader_id != 0 ? 1 : 0);
        frag_params.fog_enable = d->rs_fogenable;
        frag_params.fog_mode = d->rs_fogtablemode;
        frag_params.fog_start = d->rs_fogstart;
        frag_params.fog_end = d->rs_fogend;
        frag_params.fog_density = d->rs_fogdensity;
        frag_params.fog_color[0] = (float)((d->rs_fogcolor >> 16) & 0xFFu) / 255.0f;
        frag_params.fog_color[1] = (float)((d->rs_fogcolor >> 8) & 0xFFu) / 255.0f;
        frag_params.fog_color[2] = (float)(d->rs_fogcolor & 0xFFu) / 255.0f;
        frag_params.fog_color[3] = (float)((d->rs_fogcolor >> 24) & 0xFFu) / 255.0f;
        frag_params.ps_c0[0] = 1.0f;
        frag_params.ps_c0[1] = 1.0f;
        frag_params.ps_c0[2] = 1.0f;
        frag_params.ps_c0[3] = 1.0f;
        if (d->pixel_shader_id != 0 && d->ps_constants_size >= 16) {
          const float *ps_data = (const float *)(ipc_base + bulk_off +
                                                  d->ps_constants_bulk_offset);
          frag_params.ps_c0[0] = ps_data[0];
          frag_params.ps_c0[1] = ps_data[1];
          frag_params.ps_c0[2] = ps_data[2];
          frag_params.ps_c0[3] = ps_data[3];
        }
        if (draw_texture) {
          [encoder setFragmentBytes:&frag_params
                             length:sizeof(frag_params)
                            atIndex:0];
          [encoder setFragmentTexture:draw_texture atIndex:0];
          [encoder setFragmentSamplerState:draw_sampler atIndex:0];
        } else {
          [encoder setFragmentBytes:&frag_params
                             length:sizeof(frag_params)
                            atIndex:0];
          [encoder setFragmentTexture:nil atIndex:0];
          [encoder setFragmentSamplerState:nil atIndex:0];
        }

        /* Bind VS constants at buffer index 1 for the WVP matrix */
        if (decl_has_positiont && d->viewport_width > 0 && d->viewport_height > 0) {
          float vpX = (float)d->viewport_x;
          float vpY = (float)d->viewport_y;
          float vpW = (float)d->viewport_width;
          float vpH = (float)d->viewport_height;
          float pt_matrix[16] = {
            2.0f / vpW,              0.0f,                   0.0f, 0.0f,
            0.0f,                   -2.0f / vpH,             0.0f, 0.0f,
            0.0f,                    0.0f,                   1.0f, 0.0f,
           -2.0f * vpX / vpW - 1.0f, 2.0f * vpY / vpH + 1.0f, 0.0f, 1.0f,
          };
          [encoder setVertexBytes:pt_matrix length:sizeof(pt_matrix) atIndex:1];
        } else if (d->vs_constants_size >= 64) {
          const void *vs_data =
              (const void *)(ipc_base + bulk_off + d->vs_constants_bulk_offset);
          [encoder setVertexBytes:vs_data length:d->vs_constants_size atIndex:1];
        } else {
          static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                              0, 0, 1, 0, 0, 0, 0, 1};
          [encoder setVertexBytes:identity length:sizeof(identity) atIndex:1];
        }
      }

      /* RB4: set depth/stencil state per draw */
      {
        id<MTLDepthStencilState> ds_state = depth_stencil_state_for_draw(d);
        if (ds_state) {
          [encoder setDepthStencilState:ds_state];
        }
        if (d->rs_stencilenable) {
          [encoder setStencilReferenceValue:d->rs_stencilref];
        }
      }

      /* RB5: set cull mode per draw */
      [encoder setCullMode:d3d_cull_to_mtl(d->rs_cull_mode)];

      [encoder drawIndexedPrimitives:d3d_prim_to_mtl(d->primitive_type)
                          indexCount:index_count
                           indexType:mtl_index_type
                         indexBuffer:ib_buf
                   indexBufferOffset:d->start_index *
                                    (mtl_index_type == MTLIndexTypeUInt32 ? 4
                                                                         : 2)
                       instanceCount:1
                          baseVertex:d->base_vertex
                        baseInstance:0];

      if (!target_is_drawable && d->render_target_texture_id != 0) {
        [s_texture_rt_overrides setObject:draw_target_texture
                                   forKey:@(d->render_target_texture_id)];
      }
    }

    /* Overlay bar (draw count indicator from RB1) */
    if (s_overlay_pso && draw_count > 0 && s_width > 0 && s_height > 0) {
      if (!encoder || !active_target_is_drawable) {
        MTLRenderPassDescriptor *overlay_desc;
        BOOL seen_drawable = [cleared_targets containsObject:drawable_key];

        if (encoder) {
          [encoder endEncoding];
        }
        overlay_desc = [MTLRenderPassDescriptor renderPassDescriptor];
        overlay_desc.colorAttachments[0].texture = drawable.texture;
        overlay_desc.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (seen_drawable) {
          overlay_desc.colorAttachments[0].loadAction = MTLLoadActionLoad;
        } else {
          overlay_desc.colorAttachments[0].loadAction = MTLLoadActionClear;
          overlay_desc.colorAttachments[0].clearColor = MTLClearColorMake(r, g, b, a);
          [cleared_targets addObject:drawable_key];
        }
        /* RB4: overlay depth attachment (required by PSO, but unused) */
        {
          id<MTLTexture> overlay_depth =
              ensure_drawable_depth_texture(s_width, s_height);
          if (overlay_depth) {
            overlay_desc.depthAttachment.texture = overlay_depth;
            overlay_desc.depthAttachment.loadAction = MTLLoadActionDontCare;
            overlay_desc.depthAttachment.storeAction = MTLStoreActionDontCare;
          }
        }
        encoder = [cmd_buf renderCommandEncoderWithDescriptor:overlay_desc];
        active_target_is_drawable = 1;
        active_rt_id = 0;
      }
      if (!encoder) {
        return;
      }

      float bar_width_px = (float)draw_count;
      float bar_height_px = 8.0f;
      if (bar_width_px > (float)s_width) {
        bar_width_px = (float)s_width;
      }

      /* Reset viewport to full screen for overlay */
      MTLViewport full_vp = {0, 0, (double)s_width, (double)s_height, 0, 1};
      [encoder setViewport:full_vp];

      float ndc_x = -1.0f;
      float ndc_y = 1.0f - (bar_height_px / (float)s_height) * 2.0f;
      float ndc_w = (bar_width_px / (float)s_width) * 2.0f;
      float ndc_h = (bar_height_px / (float)s_height) * 2.0f;

      float bar_r = ((hdr->replay_hash >> 16) & 0xFFu) / 255.0f;
      float bar_g = ((hdr->replay_hash >> 8) & 0xFFu) / 255.0f;
      float bar_b = (hdr->replay_hash & 0xFFu) / 255.0f;

      struct {
        float color[4];
        float rect[4];
      } constants = {{bar_r, bar_g, bar_b, 0.9f},
                     {ndc_x, ndc_y, ndc_w, ndc_h}};

      [encoder setRenderPipelineState:s_overlay_pso];
      if (s_no_depth_state) {
        [encoder setDepthStencilState:s_no_depth_state];
      }
      [encoder setVertexBytes:&constants
                       length:sizeof(constants)
                      atIndex:0];
      [encoder setFragmentBytes:&constants
                         length:sizeof(constants)
                        atIndex:0];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                  vertexStart:0
                  vertexCount:4];
    }

    if (encoder) {
      [encoder endEncoding];
    }
    [cmd_buf presentDrawable:drawable];
    [cmd_buf commit];
  }
}

@interface DX9MTViewerDelegate : NSObject <NSApplicationDelegate> {
  const volatile unsigned char *_ipc_base;
  uint32_t _last_seq;
  NSTimer *_timer;
}
@end

@implementation DX9MTViewerDelegate

- (instancetype)initWithIPC:(const volatile unsigned char *)base {
  self = [super init];
  if (self) {
    _ipc_base = base;
    _last_seq = 0;
  }
  return self;
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
  (void)notification;
  create_window(s_width, s_height);
  _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 120.0
                                            target:self
                                          selector:@selector(pollAndRender)
                                          userInfo:nil
                                           repeats:YES];
  /* Press 'D' in the viewer window to dump the next frame. */
  [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                        handler:^NSEvent *(NSEvent *event) {
    if ([[event charactersIgnoringModifiers] isEqualToString:@"d"]) {
      s_dump_next_frame = 1;
      fprintf(stderr,
              "dx9mt_metal_viewer: frame dump requested (next frame)\n");
    } else if ([[event charactersIgnoringModifiers] isEqualToString:@"f"]) {
      if (s_dump_continuous) {
        s_dump_continuous = 0;
        fprintf(stderr,
                "dx9mt_metal_viewer: continuous dump OFF (%u frames captured)\n",
                s_dump_seq);
      } else {
        s_dump_seq = 0;
        s_dump_continuous = 1;
        fprintf(stderr,
                "dx9mt_metal_viewer: continuous dump ON "
                "(writing to %s/dx9mt_frame_dump_NNNN.txt)\n",
                dx9mt_output_dir());
      }
    }
    return event;
  }];
}

- (void)pollAndRender {
  const volatile dx9mt_metal_ipc_header *hdr =
      (const volatile dx9mt_metal_ipc_header *)_ipc_base;
  uint32_t seq = __atomic_load_n(&hdr->sequence, __ATOMIC_ACQUIRE);
  if (seq == _last_seq || seq == 0) {
    return;
  }
  _last_seq = seq;

  uint32_t w = hdr->width;
  uint32_t h = hdr->height;
  if (w > 0 && h > 0 && (w != s_width || h != s_height)) {
    create_window(w, h);
  }

  if (s_dump_next_frame) {
    s_dump_next_frame = 0;
    dump_frame(_ipc_base);
  }
  if (s_dump_continuous) {
    char name[64];
    char path[PATH_MAX];
    snprintf(name, sizeof(name), "dx9mt_frame_dump_%04u.txt", s_dump_seq++);
    build_output_path(path, sizeof(path), name);
    dump_frame_to(_ipc_base, path);
  }

  render_frame(_ipc_base);
}


- (void)applicationWillTerminate:(NSNotification *)notification {
  (void)notification;
  [_timer invalidate];
  _timer = nil;
}

@end

int main(int argc, const char *argv[]) {
  int fd;
  struct stat st;
  void *mapped;

  (void)argc;
  (void)argv;

  fd = open(DX9MT_METAL_IPC_PATH, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr,
            "dx9mt_metal_viewer: cannot open %s (create it before launching)\n",
            DX9MT_METAL_IPC_PATH);
    return 1;
  }

  if (fstat(fd, &st) != 0 || (size_t)st.st_size < DX9MT_METAL_IPC_SIZE) {
    fprintf(stderr, "dx9mt_metal_viewer: shared file too small (%lld < %u)\n",
            (long long)st.st_size, DX9MT_METAL_IPC_SIZE);
    close(fd);
    return 1;
  }

  mapped = mmap(NULL, DX9MT_METAL_IPC_SIZE, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (mapped == MAP_FAILED) {
    fprintf(stderr, "dx9mt_metal_viewer: mmap failed\n");
    return 1;
  }

  if (init_metal() != 0) {
    return 1;
  }

  @autoreleasepool {
    NSApplication *app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    DX9MTViewerDelegate *delegate =
        [[DX9MTViewerDelegate alloc] initWithIPC:(const volatile unsigned char *)mapped];
    [app setDelegate:delegate];
    [app run];
  }

  munmap(mapped, DX9MT_METAL_IPC_SIZE);
  return 0;
}
