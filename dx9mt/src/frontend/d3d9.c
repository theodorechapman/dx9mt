#include <windows.h>

#define INITGUID
#include <d3d9.h>

#include <string.h>

#include "dx9mt/d3d9_device.h"
#include "dx9mt/log.h"
#include "dx9mt/runtime.h"

typedef struct dx9mt_d3d9 {
  IDirect3D9 iface;
  LONG refcount;
  UINT sdk_version;
} dx9mt_d3d9;

static HRESULT WINAPI dx9mt_d3d9_QueryInterface(IDirect3D9 *iface, REFIID riid,
                                                 void **ppv_object);
static ULONG WINAPI dx9mt_d3d9_AddRef(IDirect3D9 *iface);
static ULONG WINAPI dx9mt_d3d9_Release(IDirect3D9 *iface);
static HRESULT WINAPI dx9mt_d3d9_RegisterSoftwareDevice(IDirect3D9 *iface,
                                                         void *initialize);
static UINT WINAPI dx9mt_d3d9_GetAdapterCount(IDirect3D9 *iface);
static HRESULT WINAPI dx9mt_d3d9_GetAdapterIdentifier(
    IDirect3D9 *iface, UINT adapter, DWORD flags,
    D3DADAPTER_IDENTIFIER9 *identifier);
static UINT WINAPI dx9mt_d3d9_GetAdapterModeCount(IDirect3D9 *iface, UINT adapter,
                                                   D3DFORMAT format);
static HRESULT WINAPI dx9mt_d3d9_EnumAdapterModes(IDirect3D9 *iface, UINT adapter,
                                                   D3DFORMAT format, UINT mode,
                                                   D3DDISPLAYMODE *display_mode);
static HRESULT WINAPI dx9mt_d3d9_GetAdapterDisplayMode(
    IDirect3D9 *iface, UINT adapter, D3DDISPLAYMODE *display_mode);
static HRESULT WINAPI dx9mt_d3d9_CheckDeviceType(
    IDirect3D9 *iface, UINT adapter, D3DDEVTYPE device_type,
    D3DFORMAT display_format, D3DFORMAT backbuffer_format, WINBOOL windowed);
static HRESULT WINAPI dx9mt_d3d9_CheckDeviceFormat(
    IDirect3D9 *iface, UINT adapter, D3DDEVTYPE device_type,
    D3DFORMAT adapter_format, DWORD usage, D3DRESOURCETYPE resource_type,
    D3DFORMAT check_format);
static HRESULT WINAPI dx9mt_d3d9_CheckDeviceMultiSampleType(
    IDirect3D9 *iface, UINT adapter, D3DDEVTYPE device_type,
    D3DFORMAT surface_format, WINBOOL windowed,
    D3DMULTISAMPLE_TYPE multisample_type, DWORD *quality_levels);
static HRESULT WINAPI dx9mt_d3d9_CheckDepthStencilMatch(
    IDirect3D9 *iface, UINT adapter, D3DDEVTYPE device_type,
    D3DFORMAT adapter_format, D3DFORMAT rt_format, D3DFORMAT ds_format);
static HRESULT WINAPI dx9mt_d3d9_CheckDeviceFormatConversion(
    IDirect3D9 *iface, UINT adapter, D3DDEVTYPE device_type,
    D3DFORMAT source_format, D3DFORMAT target_format);
static HRESULT WINAPI dx9mt_d3d9_GetDeviceCaps(IDirect3D9 *iface, UINT adapter,
                                                D3DDEVTYPE device_type,
                                                D3DCAPS9 *caps);
static HMONITOR WINAPI dx9mt_d3d9_GetAdapterMonitor(IDirect3D9 *iface,
                                                    UINT adapter);
static HRESULT WINAPI dx9mt_d3d9_CreateDevice(
    IDirect3D9 *iface, UINT adapter, D3DDEVTYPE device_type, HWND focus_window,
    DWORD behavior_flags, D3DPRESENT_PARAMETERS *presentation_parameters,
    IDirect3DDevice9 **returned_device_interface);

static const IDirect3D9Vtbl g_dx9mt_d3d9_vtbl = {
    dx9mt_d3d9_QueryInterface,
    dx9mt_d3d9_AddRef,
    dx9mt_d3d9_Release,
    dx9mt_d3d9_RegisterSoftwareDevice,
    dx9mt_d3d9_GetAdapterCount,
    dx9mt_d3d9_GetAdapterIdentifier,
    dx9mt_d3d9_GetAdapterModeCount,
    dx9mt_d3d9_EnumAdapterModes,
    dx9mt_d3d9_GetAdapterDisplayMode,
    dx9mt_d3d9_CheckDeviceType,
    dx9mt_d3d9_CheckDeviceFormat,
    dx9mt_d3d9_CheckDeviceMultiSampleType,
    dx9mt_d3d9_CheckDepthStencilMatch,
    dx9mt_d3d9_CheckDeviceFormatConversion,
    dx9mt_d3d9_GetDeviceCaps,
    dx9mt_d3d9_GetAdapterMonitor,
    dx9mt_d3d9_CreateDevice,
};

static WINBOOL dx9mt_is_color_format(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
  case D3DFMT_X8R8G8B8:
  case D3DFMT_A8R8G8B8:
    return TRUE;
  default:
    return FALSE;
  }
}

static WINBOOL dx9mt_is_render_target_format(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8:
  case D3DFMT_A2R10G10B10:
  case D3DFMT_A8B8G8R8:
  case D3DFMT_X8B8G8R8:
  case D3DFMT_G16R16:
  case D3DFMT_A2B10G10R10:
  case D3DFMT_A16B16G16R16:
  case D3DFMT_R16F:
  case D3DFMT_G16R16F:
  case D3DFMT_A16B16G16R16F:
  case D3DFMT_R32F:
  case D3DFMT_G32R32F:
  case D3DFMT_A32B32G32R32F:
    return TRUE;
  default:
    return FALSE;
  }
}

static WINBOOL dx9mt_is_supported_device_type(D3DDEVTYPE device_type) {
  return device_type == D3DDEVTYPE_HAL || device_type == D3DDEVTYPE_REF;
}

static WINBOOL dx9mt_is_depth_format(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_D16:
  case D3DFMT_D24X8:
  case D3DFMT_D24S8:
    return TRUE;
  default:
    return FALSE;
  }
}

static dx9mt_d3d9 *dx9mt_d3d9_create(UINT sdk_version) {
  dx9mt_d3d9 *object =
      (dx9mt_d3d9 *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                              sizeof(dx9mt_d3d9));
  if (!object) {
    return NULL;
  }

  object->iface.lpVtbl = (IDirect3D9Vtbl *)&g_dx9mt_d3d9_vtbl;
  object->refcount = 1;
  object->sdk_version = sdk_version;
  return object;
}

static HRESULT WINAPI dx9mt_d3d9_QueryInterface(IDirect3D9 *iface, REFIID riid,
                                                 void **ppv_object) {
  if (!ppv_object) {
    return E_POINTER;
  }

  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3D9)) {
    *ppv_object = iface;
    dx9mt_d3d9_AddRef(iface);
    return S_OK;
  }

  *ppv_object = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI dx9mt_d3d9_AddRef(IDirect3D9 *iface) {
  dx9mt_d3d9 *self = (dx9mt_d3d9 *)iface;
  return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG WINAPI dx9mt_d3d9_Release(IDirect3D9 *iface) {
  dx9mt_d3d9 *self = (dx9mt_d3d9 *)iface;
  LONG refcount = InterlockedDecrement(&self->refcount);

  if (refcount == 0) {
    HeapFree(GetProcessHeap(), 0, self);
  }

  return (ULONG)refcount;
}

static HRESULT WINAPI dx9mt_d3d9_RegisterSoftwareDevice(IDirect3D9 *iface,
                                                         void *initialize) {
  (void)iface;
  (void)initialize;
  dx9mt_logf("d3d9", "RegisterSoftwareDevice");
  return D3D_OK;
}

static UINT WINAPI dx9mt_d3d9_GetAdapterCount(IDirect3D9 *iface) {
  (void)iface;
  dx9mt_logf("d3d9", "GetAdapterCount -> 1");
  return 1;
}

static HRESULT WINAPI dx9mt_d3d9_GetAdapterIdentifier(
    IDirect3D9 *iface, UINT adapter, DWORD flags,
    D3DADAPTER_IDENTIFIER9 *identifier) {
  (void)iface;
  (void)adapter;
  (void)flags;

  if (!identifier) {
    dx9mt_logf("d3d9", "GetAdapterIdentifier adapter=%u flags=0x%08x -> INVALIDCALL",
               adapter, (unsigned)flags);
    return D3DERR_INVALIDCALL;
  }

  memset(identifier, 0, sizeof(*identifier));
  lstrcpynA(identifier->Driver, "dx9mt", sizeof(identifier->Driver));
  lstrcpynA(identifier->Description, "dx9mt Step1 Stub Adapter",
            sizeof(identifier->Description));
  lstrcpynA(identifier->DeviceName, "dx9mt0", sizeof(identifier->DeviceName));
  identifier->VendorId = 0x106B;
  identifier->DeviceId = 0x0001;
  identifier->SubSysId = 0;
  identifier->Revision = 1;
  identifier->DeviceIdentifier = IID_IDirect3D9;
  identifier->WHQLLevel = 0;

  dx9mt_logf("d3d9",
             "GetAdapterIdentifier adapter=%u flags=0x%08x -> ok vendor=0x%04x device=0x%04x",
             adapter, (unsigned)flags, (unsigned)identifier->VendorId,
             (unsigned)identifier->DeviceId);

  return D3D_OK;
}

static UINT WINAPI dx9mt_d3d9_GetAdapterModeCount(IDirect3D9 *iface, UINT adapter,
                                                   D3DFORMAT format) {
  UINT count = 0;

  (void)iface;
  if (adapter == D3DADAPTER_DEFAULT && dx9mt_is_color_format(format)) {
    count = 1;
  }
  dx9mt_logf("d3d9", "GetAdapterModeCount adapter=%u format=%u -> %u", adapter,
             (unsigned)format, count);
  return count;
}

static HRESULT WINAPI dx9mt_d3d9_EnumAdapterModes(IDirect3D9 *iface, UINT adapter,
                                                   D3DFORMAT format, UINT mode,
                                                   D3DDISPLAYMODE *display_mode) {
  (void)iface;
  dx9mt_logf("d3d9", "EnumAdapterModes adapter=%u format=%u mode=%u", adapter,
             (unsigned)format, mode);

  if (!display_mode || adapter != D3DADAPTER_DEFAULT || mode != 0) {
    dx9mt_logf("d3d9", "EnumAdapterModes -> INVALIDCALL");
    return D3DERR_INVALIDCALL;
  }
  if (!dx9mt_is_color_format(format)) {
    dx9mt_logf("d3d9", "EnumAdapterModes -> NOTAVAILABLE");
    return D3DERR_NOTAVAILABLE;
  }

  display_mode->Width = 1280;
  display_mode->Height = 720;
  display_mode->RefreshRate = 60;
  display_mode->Format = format;
  dx9mt_logf("d3d9", "EnumAdapterModes -> ok %ux%u@%u format=%u",
             display_mode->Width, display_mode->Height,
             display_mode->RefreshRate, (unsigned)display_mode->Format);
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_d3d9_GetAdapterDisplayMode(
    IDirect3D9 *iface, UINT adapter, D3DDISPLAYMODE *display_mode) {
  dx9mt_logf("d3d9", "GetAdapterDisplayMode adapter=%u", adapter);
  return dx9mt_d3d9_EnumAdapterModes(iface, adapter, D3DFMT_X8R8G8B8, 0,
                                     display_mode);
}

static HRESULT WINAPI dx9mt_d3d9_CheckDeviceType(
    IDirect3D9 *iface, UINT adapter, D3DDEVTYPE device_type,
    D3DFORMAT display_format, D3DFORMAT backbuffer_format, WINBOOL windowed) {
  HRESULT hr = D3D_OK;

  (void)iface;

  if (adapter != D3DADAPTER_DEFAULT) {
    hr = D3DERR_INVALIDCALL;
  } else if (!dx9mt_is_supported_device_type(device_type)) {
    hr = D3DERR_NOTAVAILABLE;
  } else if (!dx9mt_is_color_format(backbuffer_format)) {
    hr = D3DERR_NOTAVAILABLE;
  } else if (windowed) {
    if (display_format != D3DFMT_UNKNOWN &&
        !dx9mt_is_color_format(display_format)) {
      hr = D3DERR_NOTAVAILABLE;
    }
  } else if (!dx9mt_is_color_format(display_format)) {
    hr = D3DERR_NOTAVAILABLE;
  }

  dx9mt_logf("d3d9",
             "CheckDeviceType adapter=%u type=%u display=%u backbuffer=%u windowed=%u -> hr=0x%08x",
             adapter, (unsigned)device_type, (unsigned)display_format,
             (unsigned)backbuffer_format, (unsigned)windowed, (unsigned)hr);
  return hr;
}

static HRESULT WINAPI dx9mt_d3d9_CheckDeviceFormat(
    IDirect3D9 *iface, UINT adapter, D3DDEVTYPE device_type,
    D3DFORMAT adapter_format, DWORD usage, D3DRESOURCETYPE resource_type,
    D3DFORMAT check_format) {
  HRESULT hr = D3D_OK;

  (void)iface;

  if (adapter != D3DADAPTER_DEFAULT) {
    hr = D3DERR_INVALIDCALL;
  } else if (!dx9mt_is_supported_device_type(device_type)) {
    hr = D3DERR_NOTAVAILABLE;
  } else if (!dx9mt_is_color_format(adapter_format)) {
    hr = D3DERR_NOTAVAILABLE;
  } else if ((usage & D3DUSAGE_DEPTHSTENCIL) != 0) {
    if (resource_type != D3DRTYPE_SURFACE || !dx9mt_is_depth_format(check_format)) {
      hr = D3DERR_NOTAVAILABLE;
    }
  } else if ((usage & D3DUSAGE_RENDERTARGET) != 0) {
    if (!dx9mt_is_render_target_format(check_format)) {
      hr = D3DERR_NOTAVAILABLE;
    }
  }

  dx9mt_logf("d3d9",
             "CheckDeviceFormat adapter=%u type=%u adapter_fmt=%u usage=0x%08x rtype=%u check_fmt=%u -> hr=0x%08x",
             adapter, (unsigned)device_type, (unsigned)adapter_format,
             (unsigned)usage, (unsigned)resource_type, (unsigned)check_format,
             (unsigned)hr);
  return hr;
}

static HRESULT WINAPI dx9mt_d3d9_CheckDeviceMultiSampleType(
    IDirect3D9 *iface, UINT adapter, D3DDEVTYPE device_type,
    D3DFORMAT surface_format, WINBOOL windowed,
    D3DMULTISAMPLE_TYPE multisample_type, DWORD *quality_levels) {
  HRESULT hr = D3DERR_NOTAVAILABLE;

  (void)iface;

  if (quality_levels) {
    *quality_levels = 0;
  }

  if (adapter != D3DADAPTER_DEFAULT) {
    hr = D3DERR_INVALIDCALL;
  } else if (!dx9mt_is_supported_device_type(device_type)) {
    hr = D3DERR_NOTAVAILABLE;
  } else if (!dx9mt_is_color_format(surface_format) &&
             !dx9mt_is_depth_format(surface_format)) {
    hr = D3DERR_NOTAVAILABLE;
  } else {
    switch (multisample_type) {
    case D3DMULTISAMPLE_NONE:
    case D3DMULTISAMPLE_NONMASKABLE:
    case D3DMULTISAMPLE_2_SAMPLES:
    case D3DMULTISAMPLE_3_SAMPLES:
    case D3DMULTISAMPLE_4_SAMPLES:
    case D3DMULTISAMPLE_5_SAMPLES:
    case D3DMULTISAMPLE_6_SAMPLES:
    case D3DMULTISAMPLE_7_SAMPLES:
    case D3DMULTISAMPLE_8_SAMPLES:
    case D3DMULTISAMPLE_9_SAMPLES:
    case D3DMULTISAMPLE_10_SAMPLES:
    case D3DMULTISAMPLE_11_SAMPLES:
    case D3DMULTISAMPLE_12_SAMPLES:
    case D3DMULTISAMPLE_13_SAMPLES:
    case D3DMULTISAMPLE_14_SAMPLES:
    case D3DMULTISAMPLE_15_SAMPLES:
    case D3DMULTISAMPLE_16_SAMPLES:
      hr = D3D_OK;
      if (quality_levels) {
        *quality_levels = 1;
      }
      break;
    default:
      hr = D3DERR_NOTAVAILABLE;
      break;
    }
  }

  /* Probe loops can be extremely large; log only useful sample points. */
  if (hr == D3D_OK || multisample_type == D3DMULTISAMPLE_NONMASKABLE ||
      multisample_type == D3DMULTISAMPLE_16_SAMPLES) {
    dx9mt_logf("d3d9",
               "CheckDeviceMultiSampleType adapter=%u type=%u fmt=%u windowed=%u ms=%u -> hr=0x%08x q=%u",
               adapter, (unsigned)device_type, (unsigned)surface_format,
               (unsigned)windowed, (unsigned)multisample_type, (unsigned)hr,
               quality_levels ? (unsigned)*quality_levels : 0u);
  }

  return hr;
}

static HRESULT WINAPI dx9mt_d3d9_CheckDepthStencilMatch(
    IDirect3D9 *iface, UINT adapter, D3DDEVTYPE device_type,
    D3DFORMAT adapter_format, D3DFORMAT rt_format, D3DFORMAT ds_format) {
  HRESULT hr = D3D_OK;

  (void)iface;

  if (adapter != D3DADAPTER_DEFAULT) {
    hr = D3DERR_INVALIDCALL;
  } else if (!dx9mt_is_supported_device_type(device_type)) {
    hr = D3DERR_NOTAVAILABLE;
  } else if (!dx9mt_is_color_format(adapter_format) ||
             !dx9mt_is_render_target_format(rt_format) ||
             !dx9mt_is_depth_format(ds_format)) {
    hr = D3DERR_NOTAVAILABLE;
  }

  dx9mt_logf("d3d9",
             "CheckDepthStencilMatch adapter=%u type=%u adapter_fmt=%u rt_fmt=%u ds_fmt=%u -> hr=0x%08x",
             adapter, (unsigned)device_type, (unsigned)adapter_format,
             (unsigned)rt_format, (unsigned)ds_format, (unsigned)hr);
  return hr;
}

static HRESULT WINAPI dx9mt_d3d9_CheckDeviceFormatConversion(
    IDirect3D9 *iface, UINT adapter, D3DDEVTYPE device_type,
    D3DFORMAT source_format, D3DFORMAT target_format) {
  (void)iface;
  (void)adapter;
  (void)device_type;
  (void)source_format;
  (void)target_format;
  dx9mt_logf("d3d9",
             "CheckDeviceFormatConversion adapter=%u type=%u src_fmt=%u dst_fmt=%u -> ok",
             adapter, (unsigned)device_type, (unsigned)source_format,
             (unsigned)target_format);
  return D3D_OK;
}

static HRESULT WINAPI dx9mt_d3d9_GetDeviceCaps(IDirect3D9 *iface, UINT adapter,
                                                D3DDEVTYPE device_type,
                                                D3DCAPS9 *caps) {
  (void)iface;

  if (!caps) {
    dx9mt_logf("d3d9", "GetDeviceCaps adapter=%u type=%u -> INVALIDCALL", adapter,
               (unsigned)device_type);
    return D3DERR_INVALIDCALL;
  }
  if (adapter != D3DADAPTER_DEFAULT ||
      !dx9mt_is_supported_device_type(device_type)) {
    dx9mt_logf("d3d9",
               "GetDeviceCaps adapter=%u type=%u -> NOTAVAILABLE",
               adapter, (unsigned)device_type);
    return D3DERR_NOTAVAILABLE;
  }

  memset(caps, 0, sizeof(*caps));
  caps->AdapterOrdinal = adapter;
  caps->DeviceType = device_type;
  caps->Caps = D3DCAPS_READ_SCANLINE;
  caps->Caps2 = D3DCAPS2_CANAUTOGENMIPMAP | D3DCAPS2_FULLSCREENGAMMA |
                D3DCAPS2_DYNAMICTEXTURES;
  caps->Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD |
                D3DCAPS3_COPY_TO_VIDMEM | D3DCAPS3_COPY_TO_SYSTEMMEM;
  caps->PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE |
                                D3DPRESENT_INTERVAL_ONE |
                                D3DPRESENT_INTERVAL_TWO |
                                D3DPRESENT_INTERVAL_THREE |
                                D3DPRESENT_INTERVAL_FOUR;
  caps->CursorCaps = D3DCURSORCAPS_COLOR | D3DCURSORCAPS_LOWRES;
  caps->DevCaps = D3DDEVCAPS_EXECUTESYSTEMMEMORY |
                  D3DDEVCAPS_EXECUTEVIDEOMEMORY |
                  D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                  D3DDEVCAPS_TLVERTEXVIDEOMEMORY |
                  D3DDEVCAPS_TEXTURESYSTEMMEMORY |
                  D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                  D3DDEVCAPS_DRAWPRIMTLVERTEX |
                  D3DDEVCAPS_CANRENDERAFTERFLIP |
                  D3DDEVCAPS_TEXTURENONLOCALVIDMEM |
                  D3DDEVCAPS_DRAWPRIMITIVES2 |
                  D3DDEVCAPS_DRAWPRIMITIVES2EX |
                  D3DDEVCAPS_HWTRANSFORMANDLIGHT |
                  D3DDEVCAPS_PUREDEVICE | D3DDEVCAPS_HWRASTERIZATION;
  caps->PrimitiveMiscCaps = D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW |
                            D3DPMISCCAPS_CULLCCW |
                            D3DPMISCCAPS_COLORWRITEENABLE |
                            D3DPMISCCAPS_CLIPPLANESCALEDPOINTS |
                            D3DPMISCCAPS_BLENDOP | D3DPMISCCAPS_TSSARGTEMP;
  caps->RasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST |
                     D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE |
                     D3DPRASTERCAPS_MIPMAPLODBIAS |
                     D3DPRASTERCAPS_ZBUFFERLESSHSR | D3DPRASTERCAPS_FOGRANGE |
                     D3DPRASTERCAPS_ANISOTROPY | D3DPRASTERCAPS_COLORPERSPECTIVE |
                     D3DPRASTERCAPS_SCISSORTEST | D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS |
                     D3DPRASTERCAPS_DEPTHBIAS;
  caps->ZCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL |
                   D3DPCMPCAPS_LESSEQUAL | D3DPCMPCAPS_GREATER |
                   D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL |
                   D3DPCMPCAPS_ALWAYS;
  caps->SrcBlendCaps = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE |
                       D3DPBLENDCAPS_SRCCOLOR | D3DPBLENDCAPS_INVSRCCOLOR |
                       D3DPBLENDCAPS_SRCALPHA | D3DPBLENDCAPS_INVSRCALPHA |
                       D3DPBLENDCAPS_DESTALPHA | D3DPBLENDCAPS_INVDESTALPHA |
                       D3DPBLENDCAPS_DESTCOLOR | D3DPBLENDCAPS_INVDESTCOLOR |
                       D3DPBLENDCAPS_SRCALPHASAT | D3DPBLENDCAPS_BOTHSRCALPHA |
                       D3DPBLENDCAPS_BOTHINVSRCALPHA | D3DPBLENDCAPS_BLENDFACTOR;
  caps->DestBlendCaps = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE |
                        D3DPBLENDCAPS_SRCCOLOR | D3DPBLENDCAPS_INVSRCCOLOR |
                        D3DPBLENDCAPS_SRCALPHA | D3DPBLENDCAPS_INVSRCALPHA |
                        D3DPBLENDCAPS_DESTALPHA | D3DPBLENDCAPS_INVDESTALPHA |
                        D3DPBLENDCAPS_DESTCOLOR | D3DPBLENDCAPS_INVDESTCOLOR |
                        D3DPBLENDCAPS_SRCALPHASAT | D3DPBLENDCAPS_BLENDFACTOR;
  caps->AlphaCmpCaps = caps->ZCmpCaps;
  caps->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB |
                    D3DPSHADECAPS_SPECULARGOURAUDRGB |
                    D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGGOURAUD;
  caps->TextureCaps = D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_PERSPECTIVE |
                      D3DPTEXTURECAPS_PROJECTED |
                      D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE |
                      D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_MIPMAP;
  caps->TextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT |
                            D3DPTFILTERCAPS_MINFLINEAR |
                            D3DPTFILTERCAPS_MINFANISOTROPIC |
                            D3DPTFILTERCAPS_MIPFPOINT |
                            D3DPTFILTERCAPS_MIPFLINEAR |
                            D3DPTFILTERCAPS_MAGFPOINT |
                            D3DPTFILTERCAPS_MAGFLINEAR |
                            D3DPTFILTERCAPS_MAGFANISOTROPIC;
  caps->CubeTextureFilterCaps = caps->TextureFilterCaps;
  caps->VolumeTextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT |
                                  D3DPTFILTERCAPS_MINFLINEAR |
                                  D3DPTFILTERCAPS_MIPFPOINT |
                                  D3DPTFILTERCAPS_MIPFLINEAR |
                                  D3DPTFILTERCAPS_MAGFPOINT |
                                  D3DPTFILTERCAPS_MAGFLINEAR;
  caps->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR |
                             D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_BORDER;
  caps->VolumeTextureAddressCaps = caps->TextureAddressCaps;
  caps->LineCaps = D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST |
                   D3DLINECAPS_BLEND | D3DLINECAPS_ALPHACMP |
                   D3DLINECAPS_FOG | D3DLINECAPS_ANTIALIAS;
  caps->MaxTextureWidth = 4096;
  caps->MaxTextureHeight = 4096;
  caps->MaxVolumeExtent = 2048;
  caps->MaxTextureRepeat = 8192;
  caps->MaxTextureAspectRatio = 8192;
  caps->MaxAnisotropy = 16;
  caps->MaxVertexW = 1.0e10f;
  caps->GuardBandLeft = -8192.0f;
  caps->GuardBandTop = -8192.0f;
  caps->GuardBandRight = 8192.0f;
  caps->GuardBandBottom = 8192.0f;
  caps->ExtentsAdjust = 0.0f;
  caps->StencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO |
                      D3DSTENCILCAPS_REPLACE | D3DSTENCILCAPS_INCRSAT |
                      D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT |
                      D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR |
                      D3DSTENCILCAPS_TWOSIDED;
  caps->FVFCaps = D3DFVFCAPS_DONOTSTRIPELEMENTS | (8u << 16);
  caps->TextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 |
                        D3DTEXOPCAPS_SELECTARG2 | D3DTEXOPCAPS_MODULATE |
                        D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X |
                        D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED |
                        D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT |
                        D3DTEXOPCAPS_ADDSMOOTH |
                        D3DTEXOPCAPS_BLENDDIFFUSEALPHA |
                        D3DTEXOPCAPS_BLENDTEXTUREALPHA |
                        D3DTEXOPCAPS_BLENDFACTORALPHA |
                        D3DTEXOPCAPS_BLENDTEXTUREALPHAPM |
                        D3DTEXOPCAPS_BLENDCURRENTALPHA |
                        D3DTEXOPCAPS_PREMODULATE | D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR |
                        D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA |
                        D3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR |
                        D3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA |
                        D3DTEXOPCAPS_BUMPENVMAP |
                        D3DTEXOPCAPS_BUMPENVMAPLUMINANCE |
                        D3DTEXOPCAPS_DOTPRODUCT3 | D3DTEXOPCAPS_MULTIPLYADD |
                        D3DTEXOPCAPS_LERP;
  caps->MaxTextureBlendStages = 8;
  caps->MaxSimultaneousTextures = 16;
  caps->VertexProcessingCaps = D3DVTXPCAPS_TEXGEN |
                               D3DVTXPCAPS_MATERIALSOURCE7 |
                               D3DVTXPCAPS_DIRECTIONALLIGHTS |
                               D3DVTXPCAPS_POSITIONALLIGHTS |
                               D3DVTXPCAPS_LOCALVIEWER |
                               D3DVTXPCAPS_TWEENING;
  caps->MaxActiveLights = 8;
  caps->MaxUserClipPlanes = 6;
  caps->MaxVertexBlendMatrices = 4;
  caps->MaxVertexBlendMatrixIndex = 255;
  caps->MaxPointSize = 256.0f;
  caps->MaxPrimitiveCount = 0x00ffffffu;
  caps->MaxVertexIndex = 0x00ffffffu;
  caps->MaxStreams = 16;
  caps->MaxStreamStride = 255;
  caps->VertexShaderVersion = D3DVS_VERSION(3, 0);
  caps->MaxVertexShaderConst = 256;
  caps->PixelShaderVersion = D3DPS_VERSION(3, 0);
  caps->PixelShader1xMaxValue = 3.402823466e+38f;
  caps->DevCaps2 = D3DDEVCAPS2_STREAMOFFSET |
                   D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET;
  caps->MaxNpatchTessellationLevel = 1.0f;
  caps->MasterAdapterOrdinal = adapter;
  caps->AdapterOrdinalInGroup = 0;
  caps->NumberOfAdaptersInGroup = 1;
  caps->DeclTypes = D3DDTCAPS_UBYTE4 | D3DDTCAPS_UBYTE4N |
                    D3DDTCAPS_SHORT2N | D3DDTCAPS_SHORT4N |
                    D3DDTCAPS_USHORT2N | D3DDTCAPS_USHORT4N |
                    D3DDTCAPS_UDEC3 | D3DDTCAPS_DEC3N |
                    D3DDTCAPS_FLOAT16_2 | D3DDTCAPS_FLOAT16_4;
  caps->NumSimultaneousRTs = 4;
  caps->StretchRectFilterCaps = D3DPTFILTERCAPS_MINFPOINT |
                                D3DPTFILTERCAPS_MINFLINEAR |
                                D3DPTFILTERCAPS_MAGFPOINT |
                                D3DPTFILTERCAPS_MAGFLINEAR;
  caps->VS20Caps.Caps = D3DVS20CAPS_PREDICATION;
  caps->VS20Caps.DynamicFlowControlDepth = 24;
  caps->VS20Caps.NumTemps = 32;
  caps->VS20Caps.StaticFlowControlDepth = 4;
  caps->PS20Caps.Caps = D3DPS20CAPS_ARBITRARYSWIZZLE |
                        D3DPS20CAPS_GRADIENTINSTRUCTIONS |
                        D3DPS20CAPS_PREDICATION;
  caps->PS20Caps.DynamicFlowControlDepth = 24;
  caps->PS20Caps.NumTemps = 32;
  caps->PS20Caps.StaticFlowControlDepth = 4;
  caps->PS20Caps.NumInstructionSlots = 512;
  caps->VertexTextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT |
                                  D3DPTFILTERCAPS_MINFLINEAR |
                                  D3DPTFILTERCAPS_MAGFPOINT |
                                  D3DPTFILTERCAPS_MAGFLINEAR;
  caps->MaxVShaderInstructionsExecuted = 65535;
  caps->MaxPShaderInstructionsExecuted = 65535;
  caps->MaxVertexShader30InstructionSlots = 32768;
  caps->MaxPixelShader30InstructionSlots = 32768;
  dx9mt_logf("d3d9",
             "GetDeviceCaps adapter=%u type=%u -> ok vs=0x%08x ps=0x%08x",
             adapter, (unsigned)device_type, (unsigned)caps->VertexShaderVersion,
             (unsigned)caps->PixelShaderVersion);
  return D3D_OK;
}

static HMONITOR WINAPI dx9mt_d3d9_GetAdapterMonitor(IDirect3D9 *iface,
                                                    UINT adapter) {
  POINT point = {0, 0};
  HMONITOR monitor;

  (void)iface;
  monitor = MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY);
  dx9mt_logf("d3d9", "GetAdapterMonitor adapter=%u -> %p", adapter, monitor);
  return monitor;
}

static HRESULT WINAPI dx9mt_d3d9_CreateDevice(
    IDirect3D9 *iface, UINT adapter, D3DDEVTYPE device_type, HWND focus_window,
    DWORD behavior_flags, D3DPRESENT_PARAMETERS *presentation_parameters,
    IDirect3DDevice9 **returned_device_interface) {
  dx9mt_d3d9 *self = (dx9mt_d3d9 *)iface;
  HRESULT hr;

  hr = dx9mt_device_create(iface, adapter, device_type, focus_window,
                           behavior_flags, presentation_parameters,
                           returned_device_interface);
  dx9mt_logf("d3d9", "CreateDevice adapter=%u type=%u -> hr=0x%08x", adapter,
             device_type, (unsigned)hr);
  (void)self;
  return hr;
}

__declspec(dllexport) IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk_version) {
  dx9mt_d3d9 *object;

  dx9mt_runtime_ensure_initialized();

  if (sdk_version != D3D_SDK_VERSION) {
    dx9mt_logf("d3d9", "Direct3DCreate9 rejected sdk_version=%u expected=%u",
               sdk_version, D3D_SDK_VERSION);
    return NULL;
  }

  object = dx9mt_d3d9_create(sdk_version);
  if (!object) {
    dx9mt_logf("d3d9", "Direct3DCreate9 allocation failed");
    return NULL;
  }

  dx9mt_logf("d3d9", "Direct3DCreate9 ok sdk_version=%u", sdk_version);
  return &object->iface;
}

__declspec(dllexport) HRESULT WINAPI Direct3DCreate9Ex(
    UINT sdk_version, IDirect3D9Ex **d3d9ex) {
  dx9mt_runtime_ensure_initialized();

  if (!d3d9ex) {
    return D3DERR_INVALIDCALL;
  }

  *d3d9ex = NULL;
  dx9mt_logf("d3d9", "Direct3DCreate9Ex requested sdk_version=%u (stub)",
             sdk_version);
  return D3DERR_NOTAVAILABLE;
}
