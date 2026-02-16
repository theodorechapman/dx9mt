#define COBJMACROS

#include <windows.h>

#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef D3DFMT_DXT1
#define DX9MT_MAKEFOURCC(ch0, ch1, ch2, ch3)                                      \
  ((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) |                   \
   ((uint32_t)(uint8_t)(ch2) << 16) | ((uint32_t)(uint8_t)(ch3) << 24))
#define D3DFMT_DXT1 ((D3DFORMAT)DX9MT_MAKEFOURCC('D', 'X', 'T', '1'))
#define D3DFMT_DXT3 ((D3DFORMAT)DX9MT_MAKEFOURCC('D', 'X', 'T', '3'))
#define D3DFMT_DXT5 ((D3DFORMAT)DX9MT_MAKEFOURCC('D', 'X', 'T', '5'))
#endif

#define TEST_OK 0
#define TEST_FAIL 1

#define CHECK_HR(label, expr)                                                      \
  do {                                                                             \
    hr = (expr);                                                                   \
    if (FAILED(hr)) {                                                              \
      fprintf(stderr, "FAIL [%s:%d] %s hr=0x%08lx\n", __FILE__, __LINE__, label, \
              (unsigned long)hr);                                                  \
      goto cleanup;                                                                \
    }                                                                              \
  } while (0)

#define CHECK_TRUE(label, cond)                                                    \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, label);           \
      hr = E_FAIL;                                                                 \
      goto cleanup;                                                                \
    }                                                                              \
  } while (0)

static UINT dx9mt_block_rows(UINT height) {
  UINT rows = (height + 3u) / 4u;
  return rows == 0 ? 1u : rows;
}

static UINT dx9mt_surface_size_bytes(D3DFORMAT format, UINT pitch, UINT height) {
  if (format == D3DFMT_DXT1 || format == D3DFMT_DXT3 || format == D3DFMT_DXT5) {
    return pitch * dx9mt_block_rows(height);
  }
  return pitch * height;
}

static int dx9mt_create_device(IDirect3DDevice9 **out_device) {
  IDirect3D9 *d3d = NULL;
  IDirect3DDevice9 *device = NULL;
  D3DPRESENT_PARAMETERS params;
  HRESULT hr;

  if (!out_device) {
    return TEST_FAIL;
  }
  *out_device = NULL;

  d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    fprintf(stderr, "FAIL: Direct3DCreate9 returned NULL\n");
    return TEST_FAIL;
  }

  memset(&params, 0, sizeof(params));
  params.Windowed = TRUE;
  params.SwapEffect = D3DSWAPEFFECT_DISCARD;
  params.BackBufferFormat = D3DFMT_UNKNOWN;
  params.BackBufferWidth = 64;
  params.BackBufferHeight = 64;
  params.hDeviceWindow = GetDesktopWindow();

  hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                               params.hDeviceWindow,
                               D3DCREATE_SOFTWARE_VERTEXPROCESSING |
                                   D3DCREATE_MULTITHREADED,
                               &params, &device);
  IDirect3D9_Release(d3d);
  if (FAILED(hr)) {
    fprintf(stderr, "FAIL: CreateDevice hr=0x%08lx\n", (unsigned long)hr);
    return TEST_FAIL;
  }

  *out_device = device;
  return TEST_OK;
}

static int test_dxt_subrect_copy_stays_within_blocks(void) {
  IDirect3DDevice9 *device = NULL;
  IDirect3DSurface9 *src = NULL;
  IDirect3DSurface9 *dst = NULL;
  D3DLOCKED_RECT src_lock;
  D3DLOCKED_RECT dst_lock;
  RECT src_rect;
  POINT dst_point;
  BYTE expected_first_block[8];
  UINT src_size;
  UINT dst_size;
  UINT i;
  HRESULT hr = E_FAIL;
  int rc = TEST_FAIL;

  memset(&src_lock, 0, sizeof(src_lock));
  memset(&dst_lock, 0, sizeof(dst_lock));

  if (dx9mt_create_device(&device) != TEST_OK) {
    return TEST_FAIL;
  }

  CHECK_HR("Create src surface",
           IDirect3DDevice9_CreateOffscreenPlainSurface(
               device, 16, 16, D3DFMT_DXT1, D3DPOOL_SYSTEMMEM, &src, NULL));
  CHECK_HR("Create dst surface",
           IDirect3DDevice9_CreateOffscreenPlainSurface(
               device, 16, 16, D3DFMT_DXT1, D3DPOOL_SYSTEMMEM, &dst, NULL));

  CHECK_HR("Lock src", IDirect3DSurface9_LockRect(src, &src_lock, NULL, 0));
  CHECK_HR("Lock dst", IDirect3DSurface9_LockRect(dst, &dst_lock, NULL, 0));

  src_size = dx9mt_surface_size_bytes(D3DFMT_DXT1, (UINT)src_lock.Pitch, 16);
  dst_size = dx9mt_surface_size_bytes(D3DFMT_DXT1, (UINT)dst_lock.Pitch, 16);
  CHECK_TRUE("src size matches dst size", src_size == dst_size);

  for (i = 0; i < src_size; ++i) {
    ((BYTE *)src_lock.pBits)[i] = (BYTE)(0x80u + (i & 0x3Fu));
  }
  memset(dst_lock.pBits, 0xCD, dst_size);
  memcpy(expected_first_block, src_lock.pBits, sizeof(expected_first_block));

  CHECK_HR("Unlock src", IDirect3DSurface9_UnlockRect(src));
  src_lock.pBits = NULL;
  CHECK_HR("Unlock dst", IDirect3DSurface9_UnlockRect(dst));
  dst_lock.pBits = NULL;

  src_rect.left = 0;
  src_rect.top = 0;
  src_rect.right = 4;
  src_rect.bottom = 4;
  dst_point.x = 0;
  dst_point.y = 0;
  CHECK_HR("UpdateSurface(4x4 DXT1)",
           IDirect3DDevice9_UpdateSurface(device, src, &src_rect, dst,
                                          &dst_point));

  CHECK_HR("Relock dst", IDirect3DSurface9_LockRect(dst, &dst_lock, NULL, 0));
  CHECK_TRUE("First DXT block copied",
             memcmp(dst_lock.pBits, expected_first_block,
                    sizeof(expected_first_block)) == 0);
  for (i = 8; i < dst_size; ++i) {
    if (((BYTE *)dst_lock.pBits)[i] != 0xCDu) {
      fprintf(stderr,
              "FAIL [%s:%d] DXT spill detected at byte %u (0x%02x)\n",
              __FILE__, __LINE__, i, ((BYTE *)dst_lock.pBits)[i]);
      hr = E_FAIL;
      goto cleanup;
    }
  }
  CHECK_HR("Final unlock dst", IDirect3DSurface9_UnlockRect(dst));
  dst_lock.pBits = NULL;

  rc = TEST_OK;

cleanup:
  if (src && src_lock.pBits) {
    IDirect3DSurface9_UnlockRect(src);
  }
  if (dst && dst_lock.pBits) {
    IDirect3DSurface9_UnlockRect(dst);
  }
  if (src) {
    IDirect3DSurface9_Release(src);
  }
  if (dst) {
    IDirect3DSurface9_Release(dst);
  }
  if (device) {
    IDirect3DDevice9_Release(device);
  }
  return rc;
}

static int test_dxt_rejects_misaligned_rect(void) {
  IDirect3DDevice9 *device = NULL;
  IDirect3DSurface9 *src = NULL;
  IDirect3DSurface9 *dst = NULL;
  RECT src_rect;
  POINT dst_point;
  HRESULT hr = E_FAIL;
  int rc = TEST_FAIL;

  if (dx9mt_create_device(&device) != TEST_OK) {
    return TEST_FAIL;
  }

  CHECK_HR("Create src surface",
           IDirect3DDevice9_CreateOffscreenPlainSurface(
               device, 16, 16, D3DFMT_DXT1, D3DPOOL_SYSTEMMEM, &src, NULL));
  CHECK_HR("Create dst surface",
           IDirect3DDevice9_CreateOffscreenPlainSurface(
               device, 16, 16, D3DFMT_DXT1, D3DPOOL_SYSTEMMEM, &dst, NULL));

  src_rect.left = 1;
  src_rect.top = 0;
  src_rect.right = 5;
  src_rect.bottom = 4;
  dst_point.x = 0;
  dst_point.y = 0;
  hr = IDirect3DDevice9_UpdateSurface(device, src, &src_rect, dst, &dst_point);
  CHECK_TRUE("Misaligned DXT rect rejected", hr == D3DERR_INVALIDCALL);

  rc = TEST_OK;

cleanup:
  if (src) {
    IDirect3DSurface9_Release(src);
  }
  if (dst) {
    IDirect3DSurface9_Release(dst);
  }
  if (device) {
    IDirect3DDevice9_Release(device);
  }
  return rc;
}

static int test_dxt_rejects_scaling_in_stretchrect(void) {
  IDirect3DDevice9 *device = NULL;
  IDirect3DSurface9 *src = NULL;
  IDirect3DSurface9 *dst = NULL;
  RECT src_rect;
  RECT dst_rect;
  HRESULT hr = E_FAIL;
  int rc = TEST_FAIL;

  if (dx9mt_create_device(&device) != TEST_OK) {
    return TEST_FAIL;
  }

  CHECK_HR("Create src surface",
           IDirect3DDevice9_CreateOffscreenPlainSurface(
               device, 16, 16, D3DFMT_DXT1, D3DPOOL_SYSTEMMEM, &src, NULL));
  CHECK_HR("Create dst surface",
           IDirect3DDevice9_CreateOffscreenPlainSurface(
               device, 16, 16, D3DFMT_DXT1, D3DPOOL_SYSTEMMEM, &dst, NULL));

  src_rect.left = 0;
  src_rect.top = 0;
  src_rect.right = 4;
  src_rect.bottom = 4;
  dst_rect.left = 0;
  dst_rect.top = 0;
  dst_rect.right = 8;
  dst_rect.bottom = 8;
  hr = IDirect3DDevice9_StretchRect(device, src, &src_rect, dst, &dst_rect,
                                    D3DTEXF_NONE);
  CHECK_TRUE("DXT scaling rejected by StretchRect", hr == D3DERR_INVALIDCALL);

  rc = TEST_OK;

cleanup:
  if (src) {
    IDirect3DSurface9_Release(src);
  }
  if (dst) {
    IDirect3DSurface9_Release(dst);
  }
  if (device) {
    IDirect3DDevice9_Release(device);
  }
  return rc;
}

static int test_dxt_tail_block_copy_succeeds(void) {
  IDirect3DDevice9 *device = NULL;
  IDirect3DSurface9 *src = NULL;
  IDirect3DSurface9 *dst = NULL;
  D3DLOCKED_RECT src_lock;
  D3DLOCKED_RECT dst_lock;
  POINT dst_point;
  UINT size;
  UINT i;
  HRESULT hr = E_FAIL;
  int rc = TEST_FAIL;

  memset(&src_lock, 0, sizeof(src_lock));
  memset(&dst_lock, 0, sizeof(dst_lock));

  if (dx9mt_create_device(&device) != TEST_OK) {
    return TEST_FAIL;
  }

  CHECK_HR("Create src surface",
           IDirect3DDevice9_CreateOffscreenPlainSurface(
               device, 6, 6, D3DFMT_DXT5, D3DPOOL_SYSTEMMEM, &src, NULL));
  CHECK_HR("Create dst surface",
           IDirect3DDevice9_CreateOffscreenPlainSurface(
               device, 6, 6, D3DFMT_DXT5, D3DPOOL_SYSTEMMEM, &dst, NULL));

  CHECK_HR("Lock src", IDirect3DSurface9_LockRect(src, &src_lock, NULL, 0));
  CHECK_HR("Lock dst", IDirect3DSurface9_LockRect(dst, &dst_lock, NULL, 0));
  size = dx9mt_surface_size_bytes(D3DFMT_DXT5, (UINT)src_lock.Pitch, 6);
  CHECK_TRUE("src size equals dst size",
             size == dx9mt_surface_size_bytes(D3DFMT_DXT5,
                                              (UINT)dst_lock.Pitch, 6));

  for (i = 0; i < size; ++i) {
    ((BYTE *)src_lock.pBits)[i] = (BYTE)(i ^ 0x5Au);
  }
  memset(dst_lock.pBits, 0xEE, size);
  CHECK_HR("Unlock src", IDirect3DSurface9_UnlockRect(src));
  src_lock.pBits = NULL;
  CHECK_HR("Unlock dst", IDirect3DSurface9_UnlockRect(dst));
  dst_lock.pBits = NULL;

  dst_point.x = 0;
  dst_point.y = 0;
  CHECK_HR("UpdateSurface(6x6 DXT5)",
           IDirect3DDevice9_UpdateSurface(device, src, NULL, dst, &dst_point));

  CHECK_HR("Relock src", IDirect3DSurface9_LockRect(src, &src_lock, NULL, 0));
  CHECK_HR("Relock dst", IDirect3DSurface9_LockRect(dst, &dst_lock, NULL, 0));
  CHECK_TRUE("Full DXT tail copy matches", memcmp(src_lock.pBits, dst_lock.pBits,
                                                  size) == 0);
  CHECK_HR("Final unlock src", IDirect3DSurface9_UnlockRect(src));
  src_lock.pBits = NULL;
  CHECK_HR("Final unlock dst", IDirect3DSurface9_UnlockRect(dst));
  dst_lock.pBits = NULL;

  rc = TEST_OK;

cleanup:
  if (src && src_lock.pBits) {
    IDirect3DSurface9_UnlockRect(src);
  }
  if (dst && dst_lock.pBits) {
    IDirect3DSurface9_UnlockRect(dst);
  }
  if (src) {
    IDirect3DSurface9_Release(src);
  }
  if (dst) {
    IDirect3DSurface9_Release(dst);
  }
  if (device) {
    IDirect3DDevice9_Release(device);
  }
  return rc;
}

static int test_linear_copy_preserved(void) {
  IDirect3DDevice9 *device = NULL;
  IDirect3DSurface9 *src = NULL;
  IDirect3DSurface9 *dst = NULL;
  D3DLOCKED_RECT src_lock;
  D3DLOCKED_RECT dst_lock;
  POINT dst_point;
  UINT size;
  UINT i;
  HRESULT hr = E_FAIL;
  int rc = TEST_FAIL;

  memset(&src_lock, 0, sizeof(src_lock));
  memset(&dst_lock, 0, sizeof(dst_lock));

  if (dx9mt_create_device(&device) != TEST_OK) {
    return TEST_FAIL;
  }

  CHECK_HR("Create src linear surface",
           IDirect3DDevice9_CreateOffscreenPlainSurface(
               device, 8, 8, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &src, NULL));
  CHECK_HR("Create dst linear surface",
           IDirect3DDevice9_CreateOffscreenPlainSurface(
               device, 8, 8, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &dst, NULL));

  CHECK_HR("Lock src", IDirect3DSurface9_LockRect(src, &src_lock, NULL, 0));
  CHECK_HR("Lock dst", IDirect3DSurface9_LockRect(dst, &dst_lock, NULL, 0));
  size = dx9mt_surface_size_bytes(D3DFMT_A8R8G8B8, (UINT)src_lock.Pitch, 8);
  CHECK_TRUE("src size equals dst size",
             size == dx9mt_surface_size_bytes(D3DFMT_A8R8G8B8,
                                              (UINT)dst_lock.Pitch, 8));

  for (i = 0; i < size; ++i) {
    ((BYTE *)src_lock.pBits)[i] = (BYTE)(i + 17u);
  }
  memset(dst_lock.pBits, 0x00, size);
  CHECK_HR("Unlock src", IDirect3DSurface9_UnlockRect(src));
  src_lock.pBits = NULL;
  CHECK_HR("Unlock dst", IDirect3DSurface9_UnlockRect(dst));
  dst_lock.pBits = NULL;

  dst_point.x = 0;
  dst_point.y = 0;
  CHECK_HR("UpdateSurface linear",
           IDirect3DDevice9_UpdateSurface(device, src, NULL, dst, &dst_point));

  CHECK_HR("Relock src", IDirect3DSurface9_LockRect(src, &src_lock, NULL, 0));
  CHECK_HR("Relock dst", IDirect3DSurface9_LockRect(dst, &dst_lock, NULL, 0));
  CHECK_TRUE("linear copy matches", memcmp(src_lock.pBits, dst_lock.pBits, size) == 0);
  CHECK_HR("Final unlock src", IDirect3DSurface9_UnlockRect(src));
  src_lock.pBits = NULL;
  CHECK_HR("Final unlock dst", IDirect3DSurface9_UnlockRect(dst));
  dst_lock.pBits = NULL;

  rc = TEST_OK;

cleanup:
  if (src && src_lock.pBits) {
    IDirect3DSurface9_UnlockRect(src);
  }
  if (dst && dst_lock.pBits) {
    IDirect3DSurface9_UnlockRect(dst);
  }
  if (src) {
    IDirect3DSurface9_Release(src);
  }
  if (dst) {
    IDirect3DSurface9_Release(dst);
  }
  if (device) {
    IDirect3DDevice9_Release(device);
  }
  return rc;
}

static int test_colorfill_rejects_block_compressed(void) {
  IDirect3DDevice9 *device = NULL;
  IDirect3DSurface9 *surface = NULL;
  HRESULT hr = E_FAIL;
  int rc = TEST_FAIL;

  if (dx9mt_create_device(&device) != TEST_OK) {
    return TEST_FAIL;
  }

  CHECK_HR("Create DXT surface",
           IDirect3DDevice9_CreateOffscreenPlainSurface(
               device, 8, 8, D3DFMT_DXT1, D3DPOOL_SYSTEMMEM, &surface, NULL));
  hr = IDirect3DDevice9_ColorFill(device, surface, NULL, 0x11223344u);
  CHECK_TRUE("ColorFill on DXT rejected", hr == D3DERR_INVALIDCALL);

  rc = TEST_OK;

cleanup:
  if (surface) {
    IDirect3DSurface9_Release(surface);
  }
  if (device) {
    IDirect3DDevice9_Release(device);
  }
  return rc;
}

int main(void) {
  if (test_dxt_subrect_copy_stays_within_blocks() != TEST_OK) {
    return 1;
  }
  if (test_dxt_rejects_misaligned_rect() != TEST_OK) {
    return 1;
  }
  if (test_dxt_rejects_scaling_in_stretchrect() != TEST_OK) {
    return 1;
  }
  if (test_dxt_tail_block_copy_succeeds() != TEST_OK) {
    return 1;
  }
  if (test_linear_copy_preserved() != TEST_OK) {
    return 1;
  }
  if (test_colorfill_rejects_block_compressed() != TEST_OK) {
    return 1;
  }
  printf("frontend_surface_copy_test: PASS\n");
  return 0;
}
