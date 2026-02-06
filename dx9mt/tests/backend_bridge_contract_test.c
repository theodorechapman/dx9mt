#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dx9mt/backend_bridge.h"
#include "dx9mt/packets.h"

static dx9mt_backend_init_desc make_init_desc(void) {
  dx9mt_backend_init_desc desc;
  memset(&desc, 0, sizeof(desc));
  desc.protocol_version = 1;
  desc.ring_capacity_bytes = 1u << 20;
  desc.upload_desc.slot_count = 8;
  desc.upload_desc.bytes_per_slot = 1u << 20;
  return desc;
}

static dx9mt_backend_present_target_desc make_target_desc(void) {
  dx9mt_backend_present_target_desc desc;
  memset(&desc, 0, sizeof(desc));
  desc.target_id = 1;
  desc.width = 1280;
  desc.height = 720;
  desc.format = 21; /* D3DFMT_X8R8G8B8 */
  desc.windowed = 1;
  return desc;
}

static dx9mt_packet_draw_indexed make_valid_draw_packet(uint32_t sequence) {
  dx9mt_packet_draw_indexed packet;
  memset(&packet, 0, sizeof(packet));
  packet.header.type = DX9MT_PACKET_DRAW_INDEXED;
  packet.header.size = (uint16_t)sizeof(packet);
  packet.header.sequence = sequence;
  packet.primitive_type = 4;
  packet.primitive_count = 1;
  packet.render_target_id = 0x05000001u;
  packet.depth_stencil_id = 0x05000002u;
  packet.vertex_buffer_id = 0x03000001u;
  packet.index_buffer_id = 0x03000002u;
  packet.vertex_decl_id = 0x0A000001u;
  packet.vertex_shader_id = 0x06000001u;
  packet.pixel_shader_id = 0x07000001u;
  packet.fvf = 0;
  packet.stream0_offset = 0;
  packet.stream0_stride = 32;
  packet.viewport_hash = 0x12345678u;
  packet.scissor_hash = 0x9ABCDEF0u;
  packet.state_block_hash = 0x0BADF00Du;
  return packet;
}

static void test_accepts_valid_packet_stream(void) {
  dx9mt_backend_init_desc init_desc;
  dx9mt_backend_present_target_desc target_desc;
  dx9mt_packet_clear clear_packet;
  dx9mt_packet_present present_packet;
  struct {
    dx9mt_packet_clear clear_packet;
    dx9mt_packet_present present_packet;
  } stream;

  init_desc = make_init_desc();
  target_desc = make_target_desc();
  assert(dx9mt_backend_bridge_init(&init_desc) == 0);
  assert(dx9mt_backend_bridge_update_present_target(&target_desc) == 0);
  assert(dx9mt_backend_bridge_begin_frame(1) == 0);

  memset(&clear_packet, 0, sizeof(clear_packet));
  clear_packet.header.type = DX9MT_PACKET_CLEAR;
  clear_packet.header.size = (uint16_t)sizeof(clear_packet);
  clear_packet.header.sequence = 1;
  clear_packet.frame_id = 1;
  clear_packet.flags = 3;
  clear_packet.color = 0x11223344u;

  memset(&present_packet, 0, sizeof(present_packet));
  present_packet.header.type = DX9MT_PACKET_PRESENT;
  present_packet.header.size = (uint16_t)sizeof(present_packet);
  present_packet.header.sequence = 2;
  present_packet.frame_id = 1;

  memset(&stream, 0, sizeof(stream));
  stream.clear_packet = clear_packet;
  stream.present_packet = present_packet;

  assert(dx9mt_backend_bridge_submit_packets(&stream.clear_packet.header,
                                             (uint32_t)sizeof(stream)) == 0);
  assert(dx9mt_backend_bridge_present(1) == 0);
  dx9mt_backend_bridge_shutdown();
}

static void test_rejects_truncated_packet(void) {
  dx9mt_backend_init_desc init_desc;
  dx9mt_backend_present_target_desc target_desc;
  dx9mt_packet_clear clear_packet;

  init_desc = make_init_desc();
  target_desc = make_target_desc();
  assert(dx9mt_backend_bridge_init(&init_desc) == 0);
  assert(dx9mt_backend_bridge_update_present_target(&target_desc) == 0);

  memset(&clear_packet, 0, sizeof(clear_packet));
  clear_packet.header.type = DX9MT_PACKET_CLEAR;
  clear_packet.header.size = (uint16_t)sizeof(clear_packet);
  clear_packet.header.sequence = 1;
  clear_packet.frame_id = 1;

  assert(dx9mt_backend_bridge_submit_packets(&clear_packet.header,
                                             (uint32_t)sizeof(clear_packet) - 1) ==
         -1);
  dx9mt_backend_bridge_shutdown();
}

static void test_rejects_non_monotonic_sequence(void) {
  dx9mt_backend_init_desc init_desc;
  dx9mt_backend_present_target_desc target_desc;
  dx9mt_packet_draw_indexed draw_packet;
  dx9mt_packet_clear clear_packet;

  init_desc = make_init_desc();
  target_desc = make_target_desc();
  assert(dx9mt_backend_bridge_init(&init_desc) == 0);
  assert(dx9mt_backend_bridge_update_present_target(&target_desc) == 0);

  draw_packet = make_valid_draw_packet(10);

  memset(&clear_packet, 0, sizeof(clear_packet));
  clear_packet.header.type = DX9MT_PACKET_CLEAR;
  clear_packet.header.size = (uint16_t)sizeof(clear_packet);
  clear_packet.header.sequence = 10; /* duplicate sequence */

  assert(dx9mt_backend_bridge_submit_packets(&draw_packet.header,
                                             (uint32_t)sizeof(draw_packet)) == 0);
  assert(dx9mt_backend_bridge_submit_packets(&clear_packet.header,
                                             (uint32_t)sizeof(clear_packet)) ==
         -1);
  dx9mt_backend_bridge_shutdown();
}

static void test_rejects_draw_with_missing_state_ids(void) {
  dx9mt_backend_init_desc init_desc;
  dx9mt_backend_present_target_desc target_desc;
  dx9mt_packet_draw_indexed draw_packet;

  init_desc = make_init_desc();
  target_desc = make_target_desc();
  assert(dx9mt_backend_bridge_init(&init_desc) == 0);
  assert(dx9mt_backend_bridge_update_present_target(&target_desc) == 0);

  draw_packet = make_valid_draw_packet(1);
  draw_packet.render_target_id = 0;

  assert(dx9mt_backend_bridge_submit_packets(&draw_packet.header,
                                             (uint32_t)sizeof(draw_packet)) ==
         -1);
  dx9mt_backend_bridge_shutdown();
}

static void test_rejects_draw_packet_with_wrong_size(void) {
  dx9mt_backend_init_desc init_desc;
  dx9mt_backend_present_target_desc target_desc;
  dx9mt_packet_draw_indexed draw_packet;

  init_desc = make_init_desc();
  target_desc = make_target_desc();
  assert(dx9mt_backend_bridge_init(&init_desc) == 0);
  assert(dx9mt_backend_bridge_update_present_target(&target_desc) == 0);

  draw_packet = make_valid_draw_packet(1);
  draw_packet.header.size = (uint16_t)(sizeof(dx9mt_packet_draw_indexed) - 4);

  assert(dx9mt_backend_bridge_submit_packets(&draw_packet.header,
                                             (uint32_t)sizeof(draw_packet)) ==
         -1);
  dx9mt_backend_bridge_shutdown();
}

static void test_rejects_present_without_target_metadata(void) {
  dx9mt_backend_init_desc init_desc;
  init_desc = make_init_desc();
  assert(dx9mt_backend_bridge_init(&init_desc) == 0);
  assert(dx9mt_backend_bridge_begin_frame(1) == 0);
  assert(dx9mt_backend_bridge_present(1) == -1);
  dx9mt_backend_bridge_shutdown();
}

int main(void) {
  test_accepts_valid_packet_stream();
  test_rejects_truncated_packet();
  test_rejects_non_monotonic_sequence();
  test_rejects_draw_with_missing_state_ids();
  test_rejects_draw_packet_with_wrong_size();
  test_rejects_present_without_target_metadata();
  puts("backend_bridge_contract_test: PASS");
  return 0;
}
