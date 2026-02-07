#ifndef DX9MT_METAL_PRESENTER_H
#define DX9MT_METAL_PRESENTER_H

#include <stdint.h>

typedef struct dx9mt_metal_present_desc {
  uint32_t clear_color_argb;
  uint32_t clear_flags;
  float clear_z;
  uint32_t clear_stencil;
  int have_clear;
  uint32_t draw_count;
  uint32_t replay_hash;
  uint32_t frame_id;
} dx9mt_metal_present_desc;

int dx9mt_metal_init(void);
int dx9mt_metal_update_target(uint32_t width, uint32_t height,
                              uint64_t target_id);
int dx9mt_metal_present(const dx9mt_metal_present_desc *desc);
void dx9mt_metal_shutdown(void);
int dx9mt_metal_is_available(void);

#endif
