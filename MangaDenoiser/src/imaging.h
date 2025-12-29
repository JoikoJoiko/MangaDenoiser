#pragma once
#include "app_state.h"
#include "filesystem.h"

CLSID GetEncoderClsid(const WCHAR* format);
CLSID EncoderForPath(const std::wstring& outPath);
bool SaveBitmap(Bitmap* bmp, const std::wstring& outPath);
std::wstring NormalizeOutputNameForSave(const std::wstring& fileName, bool preferJpegIfJpg);

std::unique_ptr<Bitmap> ResizeToWidth(Bitmap* src, int targetW);

void ApplyDenoise(Bitmap* bmp, DenoiseMode mode);

std::unique_ptr<Bitmap> BuildThumb(const std::wstring& path, bool allowImage);
void EnsureThumb(size_t i);
