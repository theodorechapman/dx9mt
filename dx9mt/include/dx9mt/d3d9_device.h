#ifndef DX9MT_D3D9_DEVICE_H
#define DX9MT_D3D9_DEVICE_H

#include <windows.h>

#include <d3d9.h>

HRESULT dx9mt_device_create(IDirect3D9 *parent, UINT adapter,
                            D3DDEVTYPE device_type, HWND focus_window,
                            DWORD behavior_flags,
                            D3DPRESENT_PARAMETERS *presentation_parameters,
                            IDirect3DDevice9 **returned_device_interface);

#endif
