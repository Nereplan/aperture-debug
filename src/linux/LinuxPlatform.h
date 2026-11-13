#pragma once

#if !defined(_WIN32)

#include "LinuxTypes.h"
#include "LinuxD2D.h"
#include "LinuxWIC.h"
#include "LinuxWin32.h"

// Linux GDI DC -> BGRA surface helper implemented in LinuxPlatformWindow.cpp
void LinuxBlitDCToBitmap(HDC hdc, uint8_t* dstBGRA, UINT dstW, UINT dstH, const RECT& rect);

#ifdef __cplusplus
extern "C" {
#endif

// Linux video window helpers shared between LinuxPlatformWindow.cpp and MpvPlayer.cpp
bool ApertureLinuxIsWayland(HWND hwnd);
uint64_t ApertureLinuxGetX11WindowId(HWND hwnd);
void ApertureLinuxPresentVideoFrame(HWND hVideoWnd, const uint8_t* bgraPixels, int width, int height);

#ifdef __cplusplus
}
#endif

#endif // !defined(_WIN32)
