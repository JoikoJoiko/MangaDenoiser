#include "worker_crop.h"
#include "app_state.h"

#define NOMINMAX
#include <windows.h>
#include <gdiplus.h>

#include <string>
#include <vector>
#include <memory>
#include <algorithm>

using namespace Gdiplus;

static inline int ClampI(int v, int a, int b) { return (v < a) ? a : (v > b) ? b : v; }

static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
    UINT num = 0, size = 0;
    if (GetImageEncodersSize(&num, &size) != Ok || size == 0) return -1;

    std::vector<BYTE> buf(size);
    ImageCodecInfo* pInfo = reinterpret_cast<ImageCodecInfo*>(buf.data());
    if (GetImageEncoders(num, size, pInfo) != Ok) return -1;

    for (UINT i = 0; i < num; i++)
    {
        if (pInfo[i].MimeType && wcscmp(pInfo[i].MimeType, format) == 0)
        {
            *pClsid = pInfo[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

static bool SavePng(Bitmap* bmp, const std::wstring& outPath)
{
    if (!bmp) return false;
    CLSID clsid{};
    if (GetEncoderClsid(L"image/png", &clsid) < 0) return false;
    return bmp->Save(outPath.c_str(), &clsid, nullptr) == Ok;
}

static bool SaveJpeg(Bitmap* bmp, const std::wstring& outPath, ULONG quality)
{
    if (!bmp) return false;
    CLSID clsid{};
    if (GetEncoderClsid(L"image/jpeg", &clsid) < 0) return false;

    quality = (quality < 1) ? 1 : (quality > 100) ? 100 : quality;

    EncoderParameters params{};
    params.Count = 1;
    params.Parameter[0].Guid = EncoderQuality;
    params.Parameter[0].Type = EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues = 1;
    params.Parameter[0].Value = &quality;

    return bmp->Save(outPath.c_str(), &clsid, &params) == Ok;
}

static std::unique_ptr<Bitmap> LoadBitmap32(const std::wstring& path)
{
    std::unique_ptr<Bitmap> src(new Bitmap(path.c_str()));
    if (!src || src->GetLastStatus() != Ok) return nullptr;

    int w = (int)src->GetWidth();
    int h = (int)src->GetHeight();
    if (w <= 0 || h <= 0) return nullptr;

    std::unique_ptr<Bitmap> out(new Bitmap(w, h, PixelFormat32bppARGB));
    if (!out || out->GetLastStatus() != Ok) return nullptr;

    Graphics g(out.get());
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.DrawImage(src.get(), 0, 0, w, h);
    return out;
}

void WorkerRunCrop(HWND hWnd)
{
    if (g_inputs.empty())
    {
        SetStatus(L"No input");
        return;
    }
    if (g_outputFolder.empty())
    {
        SetStatus(L"No output folder");
        return;
    }

    auto src = LoadBitmap32(g_inputs[0]);
    if (!src)
    {
        SetStatus(L"Failed to load image");
        return;
    }

    const int sw = (int)src->GetWidth();
    const int sh = (int)src->GetHeight();

    std::wstring base = GetFileNameOnly(g_inputs[0]);
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos) base = base.substr(0, dot);

    // Build segments from Photoshop-like guides.
    std::vector<int> guides;
    CropGetSortedGuides(sh, guides);
    std::vector<int> cuts;
    cuts.reserve(guides.size() + 2);
    cuts.push_back(0);
    for (int y : guides)
    {
        if (y > 0 && y < sh) cuts.push_back(y);
    }
    cuts.push_back(sh);
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    const int total = max(0, (int)cuts.size() - 1);
    PostMessageW(hWnd, WM_APP_PROGRESS, 0, (LPARAM)total);

    int saved = 0;

    for (int i = 0; i < total; ++i)
    {
        int y0 = ClampI(cuts[(size_t)i], 0, sh);
        int y1 = ClampI(cuts[(size_t)i + 1], 0, sh);
        if (y1 < y0) std::swap(y0, y1);

        const int x = 0;
        const int w = sw;
        const int h = y1 - y0;

        if (h <= 0)
        {
            PostMessageW(hWnd, WM_APP_PROGRESS, (WPARAM)(i + 1), (LPARAM)total);
            continue;
        }

        std::unique_ptr<Bitmap> out(new Bitmap(w, h, PixelFormat32bppARGB));
        if (!out || out->GetLastStatus() != Ok)
        {
            PostMessageW(hWnd, WM_APP_PROGRESS, (WPARAM)(i + 1), (LPARAM)total);
            continue;
        }

        Graphics g(out.get());
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetSmoothingMode(SmoothingModeHighQuality);

        g.DrawImage(
            src.get(),
            Rect(0, 0, w, h),
            x, y0, w, h,
            UnitPixel
        );

        std::wstring ext = g_cropJpeg ? L".jpg" : L".png";
        std::wstring name = base + L"_" + g_cropPrefix + PadNumber(i + 1, g_cropPad) + ext;
        std::wstring outPath = JoinPath(g_outputFolder, name);

        bool okSave = g_cropJpeg ? SaveJpeg(out.get(), outPath, (ULONG)g_cropJpegQuality)
                                : SavePng(out.get(), outPath);
        if (okSave) saved++;

        PostMessageW(hWnd, WM_APP_PROGRESS, (WPARAM)(i + 1), (LPARAM)total);
    }

    SetStatus(saved > 0 ? L"Done" : L"Done with errors");
}
