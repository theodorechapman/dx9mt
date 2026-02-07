#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Cocoa/Cocoa.h>

#include "metal_presenter.h"
#include "dx9mt/log.h"

static id<MTLDevice> s_device;
static id<MTLCommandQueue> s_queue;
static id<MTLRenderPipelineState> s_overlay_pso;
static NSWindow *s_window;
static CAMetalLayer *s_metal_layer;
static uint32_t s_target_width;
static uint32_t s_target_height;
static uint64_t s_target_id;
static int s_metal_available;
static int s_metal_initialized;

static NSString *const s_overlay_shader_source =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct OverlayConstants {\n"
     "  float4 color;\n"
     "  float4 rect;\n"
     "};\n"
     "struct VertexOut {\n"
     "  float4 position [[position]];\n"
     "};\n"
     "vertex VertexOut overlay_vertex(\n"
     "    uint vid [[vertex_id]],\n"
     "    constant OverlayConstants &c [[buffer(0)]]) {\n"
     "  float2 corners[4] = {\n"
     "    float2(c.rect.x,            c.rect.y),\n"
     "    float2(c.rect.x + c.rect.z, c.rect.y),\n"
     "    float2(c.rect.x,            c.rect.y + c.rect.w),\n"
     "    float2(c.rect.x + c.rect.z, c.rect.y + c.rect.w),\n"
     "  };\n"
     "  VertexOut out;\n"
     "  out.position = float4(corners[vid], 0.0, 1.0);\n"
     "  return out;\n"
     "}\n"
     "fragment float4 overlay_fragment(\n"
     "    VertexOut in [[stage_in]],\n"
     "    constant OverlayConstants &c [[buffer(0)]]) {\n"
     "  return c.color;\n"
     "}\n";

int dx9mt_metal_init(void) {
  NSError *error = nil;
  id<MTLLibrary> library;
  id<MTLFunction> vertex_func;
  id<MTLFunction> fragment_func;
  MTLRenderPipelineDescriptor *pso_desc;

  s_device = MTLCreateSystemDefaultDevice();
  if (!s_device) {
    dx9mt_logf("metal", "MTLCreateSystemDefaultDevice returned nil");
    s_metal_available = 0;
    s_metal_initialized = 1;
    return -1;
  }

  s_queue = [s_device newCommandQueue];
  if (!s_queue) {
    dx9mt_logf("metal", "newCommandQueue returned nil");
    s_device = nil;
    s_metal_available = 0;
    s_metal_initialized = 1;
    return -1;
  }

  library = [s_device newLibraryWithSource:s_overlay_shader_source
                                   options:nil
                                     error:&error];
  if (!library) {
    dx9mt_logf("metal", "overlay shader compile failed: %s",
               error ? [[error localizedDescription] UTF8String] : "unknown");
  } else {
    vertex_func = [library newFunctionWithName:@"overlay_vertex"];
    fragment_func = [library newFunctionWithName:@"overlay_fragment"];

    pso_desc = [[MTLRenderPipelineDescriptor alloc] init];
    pso_desc.vertexFunction = vertex_func;
    pso_desc.fragmentFunction = fragment_func;
    pso_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pso_desc.colorAttachments[0].blendingEnabled = YES;
    pso_desc.colorAttachments[0].sourceRGBBlendFactor =
        MTLBlendFactorSourceAlpha;
    pso_desc.colorAttachments[0].destinationRGBBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    pso_desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pso_desc.colorAttachments[0].destinationAlphaBlendFactor =
        MTLBlendFactorZero;

    s_overlay_pso =
        [s_device newRenderPipelineStateWithDescriptor:pso_desc error:&error];
    if (!s_overlay_pso) {
      dx9mt_logf("metal", "overlay PSO creation failed: %s",
                 error ? [[error localizedDescription] UTF8String] : "unknown");
    }
  }

  s_metal_available = 1;
  s_metal_initialized = 1;
  dx9mt_logf("metal", "init success: device=%s overlay=%s",
             [[s_device name] UTF8String], s_overlay_pso ? "yes" : "no");
  return 0;
}

int dx9mt_metal_update_target(uint32_t width, uint32_t height,
                              uint64_t target_id) {
  if (!s_metal_available) {
    return -1;
  }

  void (^create_window)(void) = ^{
    if (s_window && s_target_id == target_id) {
      [s_window setContentSize:NSMakeSize(width, height)];
      s_metal_layer.drawableSize = CGSizeMake(width, height);
    } else {
      NSRect frame;
      NSUInteger style;
      NSView *view;

      if (s_window) {
        [s_window close];
        s_window = nil;
        s_metal_layer = nil;
      }

      frame = NSMakeRect(100, 100, width, height);
      style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
              NSWindowStyleMaskResizable;
      s_window = [[NSWindow alloc] initWithContentRect:frame
                                             styleMask:style
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
      [s_window
          setTitle:[NSString stringWithFormat:@"dx9mt Metal [%ux%u]", width,
                                             height]];
      [s_window setReleasedWhenClosed:NO];

      view = [[NSView alloc] initWithFrame:frame];
      [view setWantsLayer:YES];

      s_metal_layer = [CAMetalLayer layer];
      s_metal_layer.device = s_device;
      s_metal_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
      s_metal_layer.framebufferOnly = YES;
      s_metal_layer.drawableSize = CGSizeMake(width, height);
      view.layer = s_metal_layer;

      [s_window setContentView:view];
      [s_window orderFront:nil];
    }

    s_target_width = width;
    s_target_height = height;
    s_target_id = target_id;
  };

  if ([NSThread isMainThread]) {
    create_window();
  } else {
    dispatch_sync(dispatch_get_main_queue(), create_window);
  }

  dx9mt_logf("metal", "target updated: %ux%u target_id=%llu", width, height,
             (unsigned long long)target_id);
  return 0;
}

int dx9mt_metal_present(const dx9mt_metal_present_desc *desc) {
  id<CAMetalDrawable> drawable;
  id<MTLCommandBuffer> cmd_buf;
  id<MTLRenderCommandEncoder> encoder;
  MTLRenderPassDescriptor *pass_desc;
  float r, g, b, a;

  if (!s_metal_available || !s_metal_layer || !desc) {
    return -1;
  }

  @autoreleasepool {
    drawable = [s_metal_layer nextDrawable];
    if (!drawable) {
      return -1;
    }

    cmd_buf = [s_queue commandBuffer];
    if (!cmd_buf) {
      return -1;
    }

    if (desc->have_clear) {
      a = ((desc->clear_color_argb >> 24) & 0xFFu) / 255.0f;
      r = ((desc->clear_color_argb >> 16) & 0xFFu) / 255.0f;
      g = ((desc->clear_color_argb >> 8) & 0xFFu) / 255.0f;
      b = (desc->clear_color_argb & 0xFFu) / 255.0f;
    } else {
      r = 0.0f;
      g = 0.0f;
      b = 0.0f;
      a = 1.0f;
    }

    pass_desc = [MTLRenderPassDescriptor renderPassDescriptor];
    pass_desc.colorAttachments[0].texture = drawable.texture;
    pass_desc.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass_desc.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass_desc.colorAttachments[0].clearColor = MTLClearColorMake(r, g, b, a);

    encoder = [cmd_buf renderCommandEncoderWithDescriptor:pass_desc];

    if (s_overlay_pso && desc->draw_count > 0 && s_target_width > 0 &&
        s_target_height > 0) {
      float bar_width_px = (float)desc->draw_count;
      float bar_height_px = 8.0f;
      float ndc_x, ndc_y, ndc_w, ndc_h;
      float bar_r, bar_g, bar_b;

      if (bar_width_px > (float)s_target_width) {
        bar_width_px = (float)s_target_width;
      }

      ndc_x = -1.0f;
      ndc_y = 1.0f - (bar_height_px / (float)s_target_height) * 2.0f;
      ndc_w = (bar_width_px / (float)s_target_width) * 2.0f;
      ndc_h = (bar_height_px / (float)s_target_height) * 2.0f;

      bar_r = ((desc->replay_hash >> 16) & 0xFFu) / 255.0f;
      bar_g = ((desc->replay_hash >> 8) & 0xFFu) / 255.0f;
      bar_b = (desc->replay_hash & 0xFFu) / 255.0f;

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

  return 0;
}

void dx9mt_metal_shutdown(void) {
  if (!s_metal_initialized) {
    return;
  }

  dx9mt_logf("metal", "shutdown");

  void (^teardown)(void) = ^{
    if (s_window) {
      [s_window close];
      s_window = nil;
    }
    s_metal_layer = nil;
  };

  if ([NSThread isMainThread]) {
    teardown();
  } else {
    dispatch_sync(dispatch_get_main_queue(), teardown);
  }

  s_overlay_pso = nil;
  s_queue = nil;
  s_device = nil;
  s_metal_available = 0;
  s_metal_initialized = 0;
}

int dx9mt_metal_is_available(void) { return s_metal_available; }
