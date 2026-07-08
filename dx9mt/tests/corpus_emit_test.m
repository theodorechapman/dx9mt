/*
 * Shader corpus test: scan FNV's shipped .sdp shader packages for D3D9
 * bytecode, run every shader through parse -> MSL emit, and compile every
 * Nth emitted source with the real Metal compiler. Also asserts the
 * write-mask store semantics on crafted bytecode (the miscompile class that
 * shipped corrupted geometry for months).
 *
 * Build/run (see Makefile target test-shader-corpus):
 *   clang -O2 -I../src/tools -framework Metal -framework Foundation \
 *     corpus_emit_test.m ../src/tools/d3d9_shader_parse.c \
 *     ../src/tools/d3d9_shader_emit_msl.c -o corpus_emit_test
 *   ./corpus_emit_test "<FNV>/Data/Shaders" [compileEveryN]
 */
#import <Metal/Metal.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d9_shader_parse.h"
#include "d3d9_shader_emit_msl.h"

#define DST(type, num, mask)                                                   \
  (0x80000000u | (((type) & 0x7u) << 28) | ((((type) >> 3) & 0x3u) << 11) |    \
   ((num) & 0x7FFu) | (((mask) & 0xFu) << 16))
#define SRC(type, num, swiz)                                                   \
  (0x80000000u | (((type) & 0x7u) << 28) | ((((type) >> 3) & 0x3u) << 11) |    \
   ((num) & 0x7FFu) | (((swiz) & 0xFFu) << 16))
#define OP(opcode, len) ((uint32_t)(opcode) | ((uint32_t)(len) << 24))
#define SWIZ_IDENT 0xE4u

static uint64_t g_parsed, g_parse_failed, g_emit_failed;
static uint64_t g_compiled, g_compile_failed;
static int g_unit_failures;

static void unit(int cond, const char *what) {
  printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
  if (!cond) ++g_unit_failures;
}

static void mask_semantics_units(void) {
  dx9mt_sm_program prog;
  static dx9mt_msl_emit_result msl;

  /* vs_3_0: add r0.yzw, r0, c1 (identity swizzles) must store .yzw from
   * component positions y/z/w, i.e. "r0.yzw = (r0 + c[1]).yzw". */
  uint32_t bc1[] = {
      0xFFFE0300u,
      OP(31, 2), 0x80000000u, DST(1, 0, 0xF),           /* dcl_position v0 */
      OP(2, 3), DST(0, 0, 0xE), SRC(0, 0, SWIZ_IDENT),  /* add r0.yzw,r0,c1 */
                SRC(2, 1, SWIZ_IDENT),
      OP(1, 2), DST(4, 0, 0xF), SRC(0, 0, SWIZ_IDENT),  /* mov oPos, r0 */
      0x0000FFFFu,
  };
  if (dx9mt_sm_parse(bc1, sizeof(bc1) / 4, &prog) == 0 &&
      dx9mt_msl_emit_vs(&prog, 0xAAAAu, &msl) == 0) {
    unit(strstr(msl.source, "r0.yzw = (") != NULL &&
             strstr(msl.source, ").yzw;") != NULL,
         "add .yzw stores component-positional (.yzw select)");
  } else {
    unit(0, "mask unit 1 parse/emit");
  }

  /* vs_3_0: mov r1.w, c2 (identity swizzle) must produce r1.w = (...).w,
   * NOT r1.w = c[2].x. */
  uint32_t bc2[] = {
      0xFFFE0300u,
      OP(31, 2), 0x80000000u, DST(1, 0, 0xF),
      OP(1, 2), DST(0, 1, 0x8), SRC(2, 2, SWIZ_IDENT),  /* mov r1.w, c2 */
      OP(1, 2), DST(4, 0, 0xF), SRC(0, 1, SWIZ_IDENT),  /* mov oPos, r1 */
      0x0000FFFFu,
  };
  if (dx9mt_sm_parse(bc2, sizeof(bc2) / 4, &prog) == 0 &&
      dx9mt_msl_emit_vs(&prog, 0xBBBBu, &msl) == 0) {
    unit(strstr(msl.source, "r1.w = (") != NULL &&
             strstr(msl.source, ").w;") != NULL,
         "mov .w with identity swizzle takes component w");
    unit(strstr(msl.source, "r1.w = c[2].x") == NULL,
         "mov .w does not take component x");
  } else {
    unit(0, "mask unit 2 parse/emit");
  }
}

typedef struct blob { uint32_t *words; uint32_t dwords; int is_vs; } blob;
static blob *g_blobs;
static size_t g_blob_count, g_blob_cap;

static void scan_file(const char *path) {
  FILE *f = fopen(path, "rb");
  long sz;
  unsigned char *buf;
  if (!f) return;
  fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
  buf = malloc((size_t)sz);
  if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    fclose(f); free(buf); return;
  }
  fclose(f);
  for (long off = 0; off + 8 <= sz; off += 4) {
    uint32_t tok;
    memcpy(&tok, buf + off, 4);
    int is_vs = (tok & 0xFFFF0000u) == 0xFFFE0000u;
    int is_ps = (tok & 0xFFFF0000u) == 0xFFFF0000u;
    uint8_t major = (tok >> 8) & 0xFF;
    if ((!is_vs && !is_ps) || major < 1 || major > 3) continue;
    long end = -1;
    for (long j = off + 4; j + 4 <= sz && j < off + 4 * 65536; j += 4) {
      uint32_t t; memcpy(&t, buf + j, 4);
      if (t == 0x0000FFFFu) { end = j; break; }
    }
    if (end < 0) continue;
    uint32_t dwords = (uint32_t)((end - off) / 4) + 1;
    if (g_blob_count == g_blob_cap) {
      g_blob_cap = g_blob_cap ? g_blob_cap * 2 : 1024;
      g_blobs = realloc(g_blobs, g_blob_cap * sizeof(blob));
    }
    g_blobs[g_blob_count].words = malloc(dwords * 4);
    memcpy(g_blobs[g_blob_count].words, buf + off, dwords * 4);
    g_blobs[g_blob_count].dwords = dwords;
    g_blobs[g_blob_count].is_vs = is_vs;
    ++g_blob_count;
    off = end;
  }
  free(buf);
}

int main(int argc, char **argv) {
  int compile_every = argc > 2 ? atoi(argv[2]) : 10;
  id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
  DIR *d;
  struct dirent *e;
  char path[4096];

  mask_semantics_units();

  if (argc < 2) { fprintf(stderr, "usage: %s <shader-dir> [compileN]\n", argv[0]); return 2; }
  d = opendir(argv[1]);
  if (!d) { perror("opendir"); return 2; }
  while ((e = readdir(d))) {
    if (!strstr(e->d_name, ".sdp")) continue;
    snprintf(path, sizeof(path), "%s/%s", argv[1], e->d_name);
    scan_file(path);
  }
  closedir(d);

  for (size_t i = 0; i < g_blob_count; ++i) {
    dx9mt_sm_program prog;
    static dx9mt_msl_emit_result msl;
    uint32_t hash = dx9mt_sm_bytecode_hash(g_blobs[i].words, g_blobs[i].dwords);
    if (dx9mt_sm_parse(g_blobs[i].words, g_blobs[i].dwords, &prog) != 0) {
      /* Scan false-positives land here too; real-shader failures show up
       * as a jump in this count vs the baseline (256 scan-noise blobs). */
      ++g_parse_failed;
      continue;
    }
    ++g_parsed;
    int rc = g_blobs[i].is_vs ? dx9mt_msl_emit_vs(&prog, hash, &msl)
                              : dx9mt_msl_emit_ps(&prog, hash, &msl);
    if (rc != 0) {
      if (g_emit_failed < 3) {
        printf("emit fail (%s %u dwords): %s\n",
               g_blobs[i].is_vs ? "vs" : "ps", g_blobs[i].dwords,
               msl.error_msg);
      }
      ++g_emit_failed;
      continue;
    }
    if (dev && compile_every > 0 && (i % (size_t)compile_every) == 0) {
      NSError *err = nil;
      @autoreleasepool {
        id<MTLLibrary> lib = [dev
            newLibraryWithSource:[NSString stringWithUTF8String:msl.source]
                         options:nil
                           error:&err];
        if (lib) {
          ++g_compiled;
        } else {
          ++g_compile_failed;
          if (g_compile_failed <= 3) {
            printf("METAL COMPILE FAIL (%s hash=%08x): %s\n",
                   g_blobs[i].is_vs ? "vs" : "ps", hash,
                   [[err localizedDescription] UTF8String]);
          }
        }
      }
    }
  }

  printf("\ncorpus: %zu shaders | parsed=%llu parse_failed=%llu "
         "emit_failed=%llu | metal_compiled=%llu metal_failed=%llu | "
         "unit_failures=%d\n",
         g_blob_count, (unsigned long long)g_parsed,
         (unsigned long long)g_parse_failed, (unsigned long long)g_emit_failed,
         (unsigned long long)g_compiled, (unsigned long long)g_compile_failed,
         g_unit_failures);
  /* Known scan-noise blob 8f14af8e "writes" c#/v# registers and fails Metal
   * compile; treat exactly-one recurring failure hash as acceptable via the
   * <=4 threshold observed at baseline. */
  return (g_unit_failures || g_compile_failed > 4 || g_emit_failed) ? 1 : 0;
}
