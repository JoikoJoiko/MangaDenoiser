#include "worker.h"
#include "app_state.h"
#include "worker_crop.h"

#define NOMINMAX
#include <windows.h>
#include <gdiplus.h>

#include <string>
#include <vector>
#include <memory>
#include <algorithm>

using namespace Gdiplus;

static volatile LONG g_cancelFlag = 0;

static bool EnsureDirExists(const std::wstring& dir)
{
    if (dir.empty()) return false;
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;

    size_t p = dir.find_last_of(L"\\/");
    if (p != std::wstring::npos)
    {
        std::wstring parent = dir.substr(0, p);
        if (!parent.empty()) EnsureDirExists(parent);
    }

    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

static std::wstring GetFileExtWithDot(const std::wstring& path)
{
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return L"";
    return path.substr(dot);
}

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

static bool SaveBitmap(Bitmap* bmp, const std::wstring& outPath, bool jpeg, ULONG jpegQuality = 90)
{
    if (!bmp) return false;

    CLSID clsid{};
    if (!jpeg)
    {
        if (GetEncoderClsid(L"image/png", &clsid) < 0) return false;
        return bmp->Save(outPath.c_str(), &clsid, nullptr) == Ok;
    }

    if (GetEncoderClsid(L"image/jpeg", &clsid) < 0) return false;

    EncoderParameters ep{};
    ep.Count = 1;
    ep.Parameter[0].Guid = EncoderQuality;
    ep.Parameter[0].Type = EncoderParameterValueTypeLong;
    ep.Parameter[0].NumberOfValues = 1;
    ep.Parameter[0].Value = &jpegQuality;

    return bmp->Save(outPath.c_str(), &clsid, &ep) == Ok;
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

static std::unique_ptr<Bitmap> ResizeToWidth(Bitmap* src32, int targetW)
{
    if (!src32) return nullptr;
    int sw = (int)src32->GetWidth();
    int sh = (int)src32->GetHeight();
    if (sw <= 0 || sh <= 0) return nullptr;

    if (targetW <= 0 || targetW == sw)
    {
        std::unique_ptr<Bitmap> copy(new Bitmap(sw, sh, PixelFormat32bppARGB));
        if (!copy || copy->GetLastStatus() != Ok) return nullptr;
        Graphics g(copy.get());
        g.DrawImage(src32, 0, 0, sw, sh);
        return copy;
    }

    double k = (double)targetW / (double)sw;
    int dh = (int)std::max(1.0, (double)sh * k);

    std::unique_ptr<Bitmap> out(new Bitmap(targetW, dh, PixelFormat32bppARGB));
    if (!out || out->GetLastStatus() != Ok) return nullptr;

    Graphics g(out.get());
    g.SetSmoothingMode(SmoothingModeHighQuality);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.DrawImage(src32, 0, 0, targetW, dh);
    return out;
}

static inline BYTE ClampByte(int v) { return (BYTE)((v < 0) ? 0 : (v > 255) ? 255 : v); }

static void BoxBlur3x3(std::vector<BYTE>& dst, const std::vector<BYTE>& src, int w, int h)
{
    dst.assign(src.size(), 0);

    auto idx = [w](int x, int y) { return (y * w + x) * 4; };

    for (int y = 0; y < h; ++y)
    {
        int y0 = std::max(0, y - 1);
        int y1 = y;
        int y2 = std::min(h - 1, y + 1);

        for (int x = 0; x < w; ++x)
        {
            int x0 = std::max(0, x - 1);
            int x1 = x;
            int x2 = std::min(w - 1, x + 1);

            int sumB = 0, sumG = 0, sumR = 0, sumA = 0;
            int cnt = 0;

            const int xs[3] = { x0, x1, x2 };
            const int ys[3] = { y0, y1, y2 };

            for (int yy = 0; yy < 3; ++yy)
            {
                for (int xx = 0; xx < 3; ++xx)
                {
                    int k = idx(xs[xx], ys[yy]);
                    sumB += src[k + 0];
                    sumG += src[k + 1];
                    sumR += src[k + 2];
                    sumA += src[k + 3];
                    cnt++;
                }
            }

            int d = idx(x, y);
            dst[d + 0] = (BYTE)(sumB / cnt);
            dst[d + 1] = (BYTE)(sumG / cnt);
            dst[d + 2] = (BYTE)(sumR / cnt);
            dst[d + 3] = (BYTE)(sumA / cnt);
        }
    }
}

static void Unsharp(std::vector<BYTE>& io, const std::vector<BYTE>& blurred, int amount /*0..200*/)
{
    if (io.size() != blurred.size()) return;

    const int a = std::max(0, std::min(200, amount));
    for (size_t i = 0; i < io.size(); i += 4)
    {
        int ob = io[i + 0], og = io[i + 1], orr = io[i + 2], oa = io[i + 3];
        int bb = blurred[i + 0], bg = blurred[i + 1], br = blurred[i + 2];

        int nb = ob + (a * (ob - bb)) / 100;
        int ng = og + (a * (og - bg)) / 100;
        int nr = orr + (a * (orr - br)) / 100;

        io[i + 0] = ClampByte(nb);
        io[i + 1] = ClampByte(ng);
        io[i + 2] = ClampByte(nr);
        io[i + 3] = (BYTE)oa;
    }
}

static void MangaStyle(std::vector<BYTE>& px, int w, int h)
{
    for (size_t i = 0; i < px.size(); i += 4)
    {
        int b = px[i + 0], g = px[i + 1], r = px[i + 2];
        int y = (r * 30 + g * 59 + b * 11) / 100;
        px[i + 0] = px[i + 1] = px[i + 2] = (BYTE)y;
    }

    std::vector<BYTE> tmp = px;
    auto idx = [w](int x, int y) { return (y * w + x) * 4; };

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            BYTE vals[9];
            int c = 0;

            for (int yy = std::max(0, y - 1); yy <= std::min(h - 1, y + 1); ++yy)
            {
                for (int xx = std::max(0, x - 1); xx <= std::min(w - 1, x + 1); ++xx)
                {
                    int k = idx(xx, yy);
                    vals[c++] = tmp[k];
                }
            }

            std::nth_element(vals, vals + (c / 2), vals + c);
            BYTE med = vals[c / 2];

            int d = idx(x, y);
            px[d + 0] = px[d + 1] = px[d + 2] = med;
        }
    }

    std::vector<BYTE> blur;
    BoxBlur3x3(blur, px, w, h);
    Unsharp(px, blur, 140);
}

static void ColorStyle(std::vector<BYTE>& px, int w, int h)
{
    std::vector<BYTE> blur;
    BoxBlur3x3(blur, px, w, h);
    Unsharp(px, blur, 90);
}

static void BalancedStyle(std::vector<BYTE>& px, int w, int h)
{
    std::vector<BYTE> blur;
    BoxBlur3x3(blur, px, w, h);
    Unsharp(px, blur, 115);
}

static bool ApplyDenoise(Bitmap* bmp32, DenoiseMode mode)
{
    if (!bmp32) return false;

    Rect r(0, 0, (INT)bmp32->GetWidth(), (INT)bmp32->GetHeight());
    BitmapData bd{};
    if (bmp32->LockBits(&r, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Ok)
        return false;

    const int w = (int)bd.Width;
    const int h = (int)bd.Height;
    const int stride = bd.Stride;

    std::vector<BYTE> px((size_t)w * (size_t)h * 4);
    for (int y = 0; y < h; ++y)
    {
        BYTE* row = (BYTE*)bd.Scan0 + (size_t)y * (size_t)stride;
        memcpy(px.data() + (size_t)y * (size_t)w * 4, row, (size_t)w * 4);
    }

    if (mode == DenoiseMode::Manga) MangaStyle(px, w, h);
    else if (mode == DenoiseMode::Color) ColorStyle(px, w, h);
    else BalancedStyle(px, w, h);

    for (int y = 0; y < h; ++y)
    {
        BYTE* row = (BYTE*)bd.Scan0 + (size_t)y * (size_t)stride;
        memcpy(row, px.data() + (size_t)y * (size_t)w * 4, (size_t)w * 4);
    }

    bmp32->UnlockBits(&bd);
    return true;
}

static bool DoRenameOne(const std::wstring& src, const std::wstring& outDir, int index)
{
    std::wstring ext = GetFileExtWithDot(src);
    if (ext.empty()) ext = L".dat";

    std::wstring name = g_renPrefix + PadNumber(index, g_renPad) + g_renSuffix + ext;
    std::wstring dst = JoinPath(outDir, name);

    return CopyFileW(src.c_str(), dst.c_str(), FALSE) != 0;
}

static bool DoDenoiseOne(const std::wstring& src, const std::wstring& outDir)
{
    auto bmp32 = LoadBitmap32(src);
    if (!bmp32) return false;

    auto resized = ResizeToWidth(bmp32.get(), g_outWidth);
    if (!resized) return false;

    if (!ApplyDenoise(resized.get(), g_dnMode)) return false;

    std::wstring base = GetFileNameOnly(src);
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos) base = base.substr(0, dot);

    std::wstring outPath = JoinPath(outDir, base + L"_denoised.png");
    return SaveBitmap(resized.get(), outPath, false);
}

static bool DoMergeAll(const std::vector<std::wstring>& inputs, const std::wstring& outDir, bool jpeg)
{
    if (inputs.empty()) return false;

    std::vector<std::unique_ptr<Bitmap>> imgs;
    imgs.reserve(inputs.size());

    int baseW = 0;
    for (const auto& p : inputs)
    {
        auto b = LoadBitmap32(p);
        if (!b) continue;
        if (baseW == 0) baseW = (int)b->GetWidth();
        imgs.push_back(std::move(b));
    }
    if (imgs.empty() || baseW <= 0) return false;

    long long totalH = 0;
    std::vector<int> scaledH(imgs.size(), 0);

    for (size_t i = 0; i < imgs.size(); ++i)
    {
        int w = (int)imgs[i]->GetWidth();
        int h = (int)imgs[i]->GetHeight();
        if (w <= 0 || h <= 0) continue;

        double k = (double)baseW / (double)w;
        int dh = (int)std::max(1.0, (double)h * k);
        scaledH[i] = dh;
        totalH += dh;

        if (totalH > 2000000LL)
            return false;
    }

    std::unique_ptr<Bitmap> out(new Bitmap(baseW, (INT)totalH, PixelFormat32bppARGB));
    if (!out || out->GetLastStatus() != Ok) return false;

    Graphics g(out.get());
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetSmoothingMode(SmoothingModeHighQuality);
    g.Clear(Color(255, 24, 24, 24));

    int y = 0;
    for (size_t i = 0; i < imgs.size(); ++i)
    {
        if (InterlockedCompareExchange(&g_cancelFlag, 0, 0) != 0) return false;

        int dh = scaledH[i];
        if (dh <= 0) continue;

        g.DrawImage(imgs[i].get(), Rect(0, y, baseW, dh));
        y += dh;
    }

    std::wstring ext = jpeg ? L".jpg" : L".png";
    std::wstring outPath = JoinPath(outDir, g_mergeName + ext);
    return SaveBitmap(out.get(), outPath, jpeg, 92);
}

static DWORD WINAPI WorkerThread(LPVOID param)
{
    HWND hWnd = (HWND)param;
    InterlockedExchange(&g_cancelFlag, 0);

    int expectedCropTotal = 0;
    if (g_tool == Tool::Crop && g_cropBmp && g_cropBmp->GetLastStatus() == Ok)
        expectedCropTotal = CropGetSegmentCount((int)g_cropBmp->GetHeight());
    const int total = (g_tool == Tool::Crop) ? expectedCropTotal : (int)g_inputs.size();
    ULONGLONG t0 = GetTickCount64();

    g_total = total;
    g_processed = 0;
    PostMessageW(hWnd, WM_APP_PROGRESS, 0, (LPARAM)total);

    bool ok = true;

    if (g_tool == Tool::Rename)
    {
        if (!EnsureDirExists(g_outputFolder)) ok = false;
        if (ok)
        {
            int num = g_renStart;
            for (int i = 0; i < total; ++i)
            {
                if (InterlockedCompareExchange(&g_cancelFlag, 0, 0) != 0) { ok = false; break; }

                if (!DoRenameOne(g_inputs[i], g_outputFolder, num)) ok = false;
                num++;

                g_processed = i + 1;
                PostMessageW(hWnd, WM_APP_PROGRESS, (WPARAM)g_processed, (LPARAM)total);
            }
        }
    }
    else if (g_tool == Tool::Denoise)
    {
        if (!EnsureDirExists(g_outputFolder)) ok = false;
        if (ok)
        {
            for (int i = 0; i < total; ++i)
            {
                if (InterlockedCompareExchange(&g_cancelFlag, 0, 0) != 0) { ok = false; break; }

                if (!DoDenoiseOne(g_inputs[i], g_outputFolder)) ok = false;

                g_processed = i + 1;
                PostMessageW(hWnd, WM_APP_PROGRESS, (WPARAM)g_processed, (LPARAM)total);
            }
        }
    }
    else if (g_tool == Tool::Merge)
    {
        if (!EnsureDirExists(g_mergeOutFolder)) ok = false;
        if (ok)
        {
            PostMessageW(hWnd, WM_APP_PROGRESS, 0, (LPARAM)total);
            ok = DoMergeAll(g_inputs, g_mergeOutFolder, g_mergeJpeg);

            g_processed = total;
            PostMessageW(hWnd, WM_APP_PROGRESS, (WPARAM)g_processed, (LPARAM)total);
        }
    }
    else if (g_tool == Tool::Crop)
    {
        if (!EnsureDirExists(g_outputFolder)) ok = false;

        if (ok)
        {
            PostMessageW(hWnd, WM_APP_PROGRESS, 0, (LPARAM)total);

            WorkerRunCrop(hWnd);

            g_processed = total;
            PostMessageW(hWnd, WM_APP_PROGRESS, (WPARAM)g_processed, (LPARAM)total);
        }
    }

    ULONGLONG t1 = GetTickCount64();
    g_elapsedMs = (long long)(t1 - t0);

    if (!ok)
        SetStatus(InterlockedCompareExchange(&g_cancelFlag, 0, 0) != 0 ? L"Canceled" : L"Done with errors");
    else
        SetStatus(L"Done");

    PostMessageW(hWnd, WM_APP_DONE, 0, (LPARAM)g_elapsedMs);
    return 0;
}

void StartWorker(HWND hWnd)
{
    StopWorkerIfAny();

    g_processing = true;
    g_processed = 0;

    if (g_tool == Tool::Crop && g_cropBmp && g_cropBmp->GetLastStatus() == Ok)
        g_total = CropGetSegmentCount((int)g_cropBmp->GetHeight());
    else
        g_total = (int)g_inputs.size();

    g_elapsedMs = 0;

    DWORD tid = 0;
    g_worker = CreateThread(nullptr, 0, WorkerThread, (LPVOID)hWnd, 0, &tid);
    if (!g_worker)
    {
        g_processing = false;
        SetStatus(L"Failed to start worker");
        PostMessageW(hWnd, WM_APP_DONE, 0, 0);
    }
}

void StopWorkerIfAny()
{
    if (!g_worker) return;

    InterlockedExchange(&g_cancelFlag, 1);
    DWORD wait = WaitForSingleObject(g_worker, 4000);
    if (wait == WAIT_TIMEOUT)
        TerminateThread(g_worker, 0);

    CloseHandle(g_worker);
    g_worker = nullptr;
}
