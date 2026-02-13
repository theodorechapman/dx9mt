#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dx9mt/backend_bridge.h"
#include "dx9mt/packets.h"

#define TEST_DRAW_CAPTURE_OVERFLOW_COUNT 8256u

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
  packet.texture_stage_hash = 0x01020304u;
  packet.sampler_state_hash = 0x05060708u;
  packet.stream_binding_hash = 0x0A0B0C0Du;
  packet.constants_vs.arena_index = 0;
  packet.constants_vs.offset = 0;
  packet.constants_vs.size = 4096;
  packet.constants_ps.arena_index = 0;
  packet.constants_ps.offset = 4096;
  packet.constants_ps.size = 4096;
  packet.state_block_hash = 0x0BADF00Du;
  /* RB4: depth/stencil defaults (D3D9 defaults) */
  packet.rs_zenable = 1;          /* D3DZB_TRUE */
  packet.rs_zwriteenable = 1;     /* TRUE */
  packet.rs_zfunc = 4;            /* D3DCMP_LESSEQUAL */
  packet.rs_stencilenable = 0;    /* FALSE */
  packet.rs_stencilfunc = 8;      /* D3DCMP_ALWAYS */
  packet.rs_stencilref = 0;
  packet.rs_stencilmask = 0xFFFFFFFFu;
  packet.rs_stencilwritemask = 0xFFFFFFFFu;
  packet.rs_cull_mode = 2;          /* D3DCULL_CCW */
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

static void test_rejects_present_packet_frame_mismatch(void) {
  dx9mt_backend_init_desc init_desc;
  dx9mt_backend_present_target_desc target_desc;
  dx9mt_packet_present present_packet;

  init_desc = make_init_desc();
  target_desc = make_target_desc();
  assert(dx9mt_backend_bridge_init(&init_desc) == 0);
  assert(dx9mt_backend_bridge_update_present_target(&target_desc) == 0);
  assert(dx9mt_backend_bridge_begin_frame(7) == 0);

  memset(&present_packet, 0, sizeof(present_packet));
  present_packet.header.type = DX9MT_PACKET_PRESENT;
  present_packet.header.size = (uint16_t)sizeof(present_packet);
  present_packet.header.sequence = 1;
  present_packet.frame_id = 8;
  assert(dx9mt_backend_bridge_submit_packets(&present_packet.header,
                                             (uint32_t)sizeof(present_packet)) ==
         0);
  assert(dx9mt_backend_bridge_present(7) == -1);
  dx9mt_backend_bridge_shutdown();
}

static void test_accepts_draw_capture_overflow_and_presents(void) {
  dx9mt_backend_init_desc init_desc;
  dx9mt_backend_present_target_desc target_desc;
  dx9mt_packet_draw_indexed draw_packet;
  dx9mt_packet_present present_packet;
  uint32_t i;

  init_desc = make_init_desc();
  target_desc = make_target_desc();
  assert(dx9mt_backend_bridge_init(&init_desc) == 0);
  assert(dx9mt_backend_bridge_update_present_target(&target_desc) == 0);
  assert(dx9mt_backend_bridge_begin_frame(1) == 0);

  for (i = 0; i < TEST_DRAW_CAPTURE_OVERFLOW_COUNT; ++i) {
    draw_packet = make_valid_draw_packet(i + 1);
    draw_packet.constants_vs.offset = (i * 16u) % ((1u << 20) - 4096u);
    draw_packet.constants_ps.offset =
        ((i * 16u) + 8192u) % ((1u << 20) - 4096u);
    assert(dx9mt_backend_bridge_submit_packets(&draw_packet.header,
                                               (uint32_t)sizeof(draw_packet)) ==
           0);
  }

  memset(&present_packet, 0, sizeof(present_packet));
  present_packet.header.type = DX9MT_PACKET_PRESENT;
  present_packet.header.size = (uint16_t)sizeof(present_packet);
  present_packet.header.sequence = TEST_DRAW_CAPTURE_OVERFLOW_COUNT + 1u;
  present_packet.frame_id = 1;
  assert(dx9mt_backend_bridge_submit_packets(&present_packet.header,
                                             (uint32_t)sizeof(present_packet)) ==
         0);
  assert(dx9mt_backend_bridge_present(1) == 0);
  dx9mt_backend_bridge_shutdown();
}

static void test_replay_hash_changes_with_draw_payload(void) {
  dx9mt_backend_init_desc init_desc;
  dx9mt_backend_present_target_desc target_desc;
  dx9mt_packet_draw_indexed draw_packet;
  dx9mt_packet_present present_packet;
  uint32_t first_hash;
  uint32_t second_hash;

  init_desc = make_init_desc();
  target_desc = make_target_desc();
  assert(dx9mt_backend_bridge_init(&init_desc) == 0);
  assert(dx9mt_backend_bridge_update_present_target(&target_desc) == 0);

  assert(dx9mt_backend_bridge_begin_frame(1) == 0);
  draw_packet = make_valid_draw_packet(1);
  assert(dx9mt_backend_bridge_submit_packets(&draw_packet.header,
                                             (uint32_t)sizeof(draw_packet)) ==
         0);
  memset(&present_packet, 0, sizeof(present_packet));
  present_packet.header.type = DX9MT_PACKET_PRESENT;
  present_packet.header.size = (uint16_t)sizeof(present_packet);
  present_packet.header.sequence = 2;
  present_packet.frame_id = 1;
  assert(dx9mt_backend_bridge_submit_packets(&present_packet.header,
                                             (uint32_t)sizeof(present_packet)) ==
         0);
  assert(dx9mt_backend_bridge_present(1) == 0);
  first_hash = dx9mt_backend_bridge_debug_get_last_replay_hash();
  assert(first_hash != 0);

  assert(dx9mt_backend_bridge_begin_frame(2) == 0);
  draw_packet = make_valid_draw_packet(3);
  draw_packet.texture_stage_hash ^= 0x00FF00FFu;
  draw_packet.constants_vs.offset += 256u;
  assert(dx9mt_backend_bridge_submit_packets(&draw_packet.header,
                                             (uint32_t)sizeof(draw_packet)) ==
         0);
  memset(&present_packet, 0, sizeof(present_packet));
  present_packet.header.type = DX9MT_PACKET_PRESENT;
  present_packet.header.size = (uint16_t)sizeof(present_packet);
  present_packet.header.sequence = 4;
  present_packet.frame_id = 2;
  assert(dx9mt_backend_bridge_submit_packets(&present_packet.header,
                                             (uint32_t)sizeof(present_packet)) ==
         0);
  assert(dx9mt_backend_bridge_present(2) == 0);
  second_hash = dx9mt_backend_bridge_debug_get_last_replay_hash();
  assert(second_hash != 0);
  assert(second_hash != first_hash);

  dx9mt_backend_bridge_shutdown();
}

/*
 * Verify that BEGIN_FRAME can arrive through submit_packets (the packet
 * stream) rather than only through the direct begin_frame() call. This
 * matches the frontend's new behavior where BeginScene emits a
 * BEGIN_FRAME packet, making the stream self-describing for future IPC.
 */
static void test_begin_frame_via_packet_stream(void) {
  dx9mt_backend_init_desc init_desc;
  dx9mt_backend_present_target_desc target_desc;
  dx9mt_packet_begin_frame bf_packet;
  dx9mt_packet_clear clear_packet;
  dx9mt_packet_present present_packet;
  struct {
    dx9mt_packet_begin_frame bf_packet;
    dx9mt_packet_clear clear_packet;
    dx9mt_packet_present present_packet;
  } stream;

  init_desc = make_init_desc();
  target_desc = make_target_desc();
  assert(dx9mt_backend_bridge_init(&init_desc) == 0);
  assert(dx9mt_backend_bridge_update_present_target(&target_desc) == 0);

  memset(&bf_packet, 0, sizeof(bf_packet));
  bf_packet.header.type = DX9MT_PACKET_BEGIN_FRAME;
  bf_packet.header.size = (uint16_t)sizeof(bf_packet);
  bf_packet.header.sequence = 1;
  bf_packet.frame_id = 1;

  memset(&clear_packet, 0, sizeof(clear_packet));
  clear_packet.header.type = DX9MT_PACKET_CLEAR;
  clear_packet.header.size = (uint16_t)sizeof(clear_packet);
  clear_packet.header.sequence = 2;
  clear_packet.frame_id = 1;
  clear_packet.flags = 3;
  clear_packet.color = 0xAABBCCDDu;

  memset(&present_packet, 0, sizeof(present_packet));
  present_packet.header.type = DX9MT_PACKET_PRESENT;
  present_packet.header.size = (uint16_t)sizeof(present_packet);
  present_packet.header.sequence = 3;
  present_packet.frame_id = 1;

  memset(&stream, 0, sizeof(stream));
  stream.bf_packet = bf_packet;
  stream.clear_packet = clear_packet;
  stream.present_packet = present_packet;

  assert(dx9mt_backend_bridge_submit_packets(&stream.bf_packet.header,
                                             (uint32_t)sizeof(stream)) == 0);
  assert(dx9mt_backend_bridge_present(1) == 0);
  dx9mt_backend_bridge_shutdown();
}

int main(void) {
  test_accepts_valid_packet_stream();
  test_rejects_truncated_packet();
  test_rejects_non_monotonic_sequence();
  test_rejects_draw_with_missing_state_ids();
  test_rejects_draw_packet_with_wrong_size();
  test_rejects_present_without_target_metadata();
  test_rejects_present_packet_frame_mismatch();
  test_accepts_draw_capture_overflow_and_presents();
  test_replay_hash_changes_with_draw_payload();
  test_begin_frame_via_packet_stream();
  puts("backend_bridge_contract_test: PASS");
  return 0;
}
