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

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dx9mt/metal_ipc.h"

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
  D3DDECLUSAGE_COLOR = 10,
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

static id<MTLDevice> s_device;
static id<MTLCommandQueue> s_queue;
static id<MTLRenderPipelineState> s_overlay_pso;
static id<MTLRenderPipelineState> s_geometry_pso;
static id<MTLLibrary> s_library;
static uint32_t s_geometry_pso_stride;
static CAMetalLayer *s_metal_layer;
static NSWindow *s_window;
static uint32_t s_width = 1280;
static uint32_t s_height = 720;

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
     "};\n"
     "struct GeoOut {\n"
     "  float4 position [[position]];\n"
     "  float4 color;\n"
     "};\n"
     "vertex GeoOut geo_vertex(\n"
     "    GeoIn in [[stage_in]],\n"
     "    uint vid [[vertex_id]],\n"
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
     "  /* Use vertex color if available, otherwise debug gradient. */\n"
     "  float lum = float(vid % 64u) / 63.0;\n"
     "  float3 c = in.color.rgb;\n"
     "  float color_sum = c.r + c.g + c.b;\n"
     "  if (color_sum < 0.01) {\n"
     "    c = float3(0.5 + lum * 0.3, 0.3 + lum * 0.2, 0.7);\n"
     "  }\n"
     "  out.color = float4(c, 1.0);\n"
     "  return out;\n"
     "}\n"
     "fragment float4 geo_fragment(GeoOut in [[stage_in]]) {\n"
     "  return in.color;\n"
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
    return MTLVertexFormatUChar4Normalized;
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

/*
 * Create or re-create the geometry PSO for a given vertex stride
 * and declaration. We cache by stride as a quick check -- if the
 * stride changes, we rebuild. For RB3 Phase 1 this is sufficient.
 */
static void ensure_geometry_pso(uint32_t stride,
                                const dx9mt_d3d_vertex_element *elems,
                                uint16_t elem_count) {
  NSError *error = nil;
  MTLVertexDescriptor *vd;
  MTLRenderPipelineDescriptor *desc;
  int has_position = 0;
  int has_color = 0;

  if (s_geometry_pso && s_geometry_pso_stride == stride) {
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

  vd.layouts[0].stride = stride;
  vd.layouts[0].stepRate = 1;
  vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

  desc = [[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction = [s_library newFunctionWithName:@"geo_vertex"];
  desc.fragmentFunction = [s_library newFunctionWithName:@"geo_fragment"];
  desc.vertexDescriptor = vd;
  desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

  s_geometry_pso =
      [s_device newRenderPipelineStateWithDescriptor:desc error:&error];
  if (!s_geometry_pso) {
    fprintf(stderr, "dx9mt_metal_viewer: geometry PSO failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown");
    return;
  }
  s_geometry_pso_stride = stride;
  fprintf(stderr,
          "dx9mt_metal_viewer: geometry PSO created stride=%u pos=%d col=%d\n",
          stride, has_position, has_color);
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

  /* Overlay PSO (same as RB1) */
  {
    MTLRenderPipelineDescriptor *desc =
        [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = [s_library newFunctionWithName:@"overlay_vertex"];
    desc.fragmentFunction = [s_library newFunctionWithName:@"overlay_fragment"];
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
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

    MTLRenderPassDescriptor *pass_desc =
        [MTLRenderPassDescriptor renderPassDescriptor];
    pass_desc.colorAttachments[0].texture = drawable.texture;
    pass_desc.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass_desc.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass_desc.colorAttachments[0].clearColor = MTLClearColorMake(r, g, b, a);

    id<MTLRenderCommandEncoder> encoder =
        [cmd_buf renderCommandEncoderWithDescriptor:pass_desc];

    /* RB3: Render actual geometry from per-draw IPC data */
    for (uint32_t i = 0; i < draw_count && i < DX9MT_METAL_IPC_MAX_DRAWS;
         ++i) {
      const volatile dx9mt_metal_ipc_draw *d = &draws[i];
      uint32_t stride = d->stream0_stride;
      uint32_t index_count;
      MTLIndexType mtl_index_type;

      if (d->vb_bulk_size == 0 || d->ib_bulk_size == 0 || stride == 0) {
        continue;
      }
      if (d->primitive_count == 0) {
        continue;
      }

      /* Parse vertex declaration from bulk data to build PSO */
      if (d->decl_count > 0 && d->decl_count < 64) {
        const dx9mt_d3d_vertex_element *elems =
            (const dx9mt_d3d_vertex_element *)(ipc_base + bulk_off +
                                               d->decl_bulk_offset);
        ensure_geometry_pso(stride, elems, d->decl_count);
      }

      if (!s_geometry_pso) {
        continue;
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

      /* Index type */
      if (d->index_format == D3DFMT_INDEX32) {
        mtl_index_type = MTLIndexTypeUInt32;
      } else {
        mtl_index_type = MTLIndexTypeUInt16;
      }

      index_count = d3d_index_count(d->primitive_type, d->primitive_count);

      [encoder setRenderPipelineState:s_geometry_pso];
      [encoder setVertexBuffer:vb_buf offset:d->stream0_offset atIndex:0];

      /* Bind VS constants at buffer index 1 for the WVP matrix */
      if (d->vs_constants_size >= 64) {
        const void *vs_data =
            (const void *)(ipc_base + bulk_off + d->vs_constants_bulk_offset);
        [encoder setVertexBytes:vs_data length:d->vs_constants_size atIndex:1];
      } else {
        /* No constants -- bind identity matrix as fallback */
        static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                            0, 0, 1, 0, 0, 0, 0, 1};
        [encoder setVertexBytes:identity length:sizeof(identity) atIndex:1];
      }

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
    }

    /* Overlay bar (draw count indicator from RB1) */
    if (s_overlay_pso && draw_count > 0 && s_width > 0 && s_height > 0) {
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

    [encoder endEncoding];
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
