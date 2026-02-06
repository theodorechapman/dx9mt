#include <windows.h>

#include <d3d9.h>

#include "dx9mt/log.h"

/* Legacy compatibility exports present in Wine/Windows d3d9. */
typedef struct dx9mt_d3d9on12_args {
  BOOL enable_9on12;
  IUnknown *d3d12_device;
  IUnknown *d3d12_queues[2];
  UINT num_queues;
  UINT node_mask;
} dx9mt_d3d9on12_args;

__declspec(dllexport) int WINAPI D3DPERF_BeginEvent(D3DCOLOR color,
                                                     const WCHAR *name) {
  (void)color;
  (void)name;
  return 0;
}

__declspec(dllexport) int WINAPI D3DPERF_EndEvent(void) { return 0; }

__declspec(dllexport) void WINAPI D3DPERF_SetMarker(D3DCOLOR color,
                                                     const WCHAR *name) {
  (void)color;
  (void)name;
}

__declspec(dllexport) void WINAPI D3DPERF_SetRegion(D3DCOLOR color,
                                                     const WCHAR *name) {
  (void)color;
  (void)name;
}

__declspec(dllexport) BOOL WINAPI D3DPERF_QueryRepeatFrame(void) {
  return FALSE;
}

__declspec(dllexport) void WINAPI D3DPERF_SetOptions(DWORD options) {
  (void)options;
}

__declspec(dllexport) DWORD WINAPI D3DPERF_GetStatus(void) { return 0; }

__declspec(dllexport) void WINAPI DebugSetLevel(DWORD level) {
  (void)level;
}

__declspec(dllexport) void WINAPI DebugSetMute(void) {
  dx9mt_logf("perf", "DebugSetMute called");
}

__declspec(dllexport) void *WINAPI Direct3DShaderValidatorCreate9(void) {
  dx9mt_logf("perf", "Direct3DShaderValidatorCreate9 called");
  return NULL;
}

__declspec(dllexport) void WINAPI PSGPError(void) {
  dx9mt_logf("perf", "PSGPError called");
}

__declspec(dllexport) void WINAPI PSGPSampleTexture(void) {
  dx9mt_logf("perf", "PSGPSampleTexture called");
}

__declspec(dllexport) IDirect3D9 *WINAPI Direct3DCreate9On12(
    UINT sdk_version, dx9mt_d3d9on12_args *override_list,
    UINT num_override_entries) {
  (void)override_list;
  (void)num_override_entries;
  return Direct3DCreate9(sdk_version);
}
