#include "framework.h"
#include "imaging.h"
#include <vector>
#include <cstring>

static void Lock32(Bitmap* bmp, BitmapData& bd)
{
    Rect r(0, 0, (INT)bmp->GetWidth(), (INT)bmp->GetHeight());
    bmp->LockBits(&r, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppARGB, &bd);
}
static inline int iclamp(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }

CLSID GetEncoderClsid(const WCHAR* format)
{
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return CLSID{};

    std::unique_ptr<BYTE[]> buf(new BYTE[size]);
    ImageCodecInfo* p = (ImageCodecInfo*)buf.get();
    GetImageEncoders(num, size, p);

    for (UINT i = 0; i < num; i++)
    {
        if (wcscmp(p[i].MimeType, format) == 0)
            return p[i].Clsid;
    }
    return CLSID{};
}

CLSID EncoderForPath(const std::wstring& outPath)
{
    std::wstring ext = GetExtLower(outPath);
    if (ext == L"jpg" || ext == L"jpeg") return GetEncoderClsid(L"image/jpeg");
    return GetEncoderClsid(L"image/png");
}

bool SaveBitmap(Bitmap* bmp, const std::wstring& outPath)
{
    if (!bmp || bmp->GetLastStatus() != Ok) return false;
    CLSID enc = EncoderForPath(outPath);
    if (enc == CLSID{}) return false;
    Status s = bmp->Save(outPath.c_str(), &enc, nullptr);
    return s == Ok;
}

std::wstring NormalizeOutputNameForSave(const std::wstring& fileName, bool preferJpegIfJpg)
{
    std::wstring ext = GetExtLower(fileName);
    if (ext == L"png" || ext == L"jpg" || ext == L"jpeg")
        return fileName;

    size_t dot = fileName.find_last_of(L'.');
    if (dot == std::wstring::npos)
        return fileName + (preferJpegIfJpg ? L".jpg" : L".png");

    return fileName.substr(0, dot) + (preferJpegIfJpg ? L".jpg" : L".png");
}

std::unique_ptr<Bitmap> ResizeToWidth(Bitmap* src, int targetW)
{
    if (!src || src->GetLastStatus() != Ok) return nullptr;

    int sw = (int)src->GetWidth();
    int sh = (int)src->GetHeight();
    if (sw <= 0 || sh <= 0) return nullptr;

    if (targetW <= 0) targetW = sw;
    targetW = ClampI(targetW, 1, 20000);

    float k = (float)targetW / (float)sw;
    int targetH = max(1, (int)(sh * k + 0.5f));

    std::unique_ptr<Bitmap> out(new Bitmap(targetW, targetH, PixelFormat32bppARGB));
    if (!out || out->GetLastStatus() != Ok) return nullptr;

    Graphics g(out.get());
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetSmoothingMode(SmoothingModeHighQuality);
    g.DrawImage(src, 0, 0, targetW, targetH);

    return out;
}

static void UnsharpMask32(Bitmap* bmp, float amount, int radius)
{
    if (!bmp || bmp->GetLastStatus() != Ok) return;

    BitmapData bd{};
    Lock32(bmp, bd);
    if (!bd.Scan0 || bd.Width <= 0 || bd.Height <= 0 || bd.Stride == 0)
    {
        bmp->UnlockBits(&bd);
        return;
    }

    int w = bd.Width;
    int h = bd.Height;
    int stride = bd.Stride;
    BYTE* dst0 = (BYTE*)bd.Scan0;

    size_t bufSize = (size_t)h * (size_t)stride;
    std::vector<BYTE> tmp(bufSize);
    memcpy(tmp.data(), dst0, bufSize);

    auto sp = [&](int x, int y)->BYTE* { return tmp.data() + (size_t)y * (size_t)stride + (size_t)x * 4; };
    auto dp = [&](int x, int y)->BYTE* { return dst0 + (size_t)y * (size_t)stride + (size_t)x * 4; };

    int r = max(1, radius);

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            int sumB = 0, sumG = 0, sumR = 0, cnt = 0;
            for (int oy = -r; oy <= r; oy++)
            {
                int yy = iclamp(y + oy, 0, h - 1);
                for (int ox = -r; ox <= r; ox++)
                {
                    int xx = iclamp(x + ox, 0, w - 1);
                    BYTE* p = sp(xx, yy);
                    sumB += p[0]; sumG += p[1]; sumR += p[2];
                    cnt++;
                }
            }

            BYTE* o = dp(x, y);
            int blurB = sumB / cnt;
            int blurG = sumG / cnt;
            int blurR = sumR / cnt;

            int b = (int)o[0], g = (int)o[1], rr = (int)o[2];

            int nb = iclamp((int)(b + (b - blurB) * amount), 0, 255);
            int ng = iclamp((int)(g + (g - blurG) * amount), 0, 255);
            int nr = iclamp((int)(rr + (rr - blurR) * amount), 0, 255);

            o[0] = (BYTE)nb; o[1] = (BYTE)ng; o[2] = (BYTE)nr;
        }

    bmp->UnlockBits(&bd);
}

static void Median3x3Luma(Bitmap* bmp, int strength)
{
    if (!bmp || bmp->GetLastStatus() != Ok) return;

    BitmapData bd{};
    Lock32(bmp, bd);
    if (!bd.Scan0 || bd.Width <= 0 || bd.Height <= 0 || bd.Stride == 0)
    {
        bmp->UnlockBits(&bd);
        return;
    }

    int w = bd.Width;
    int h = bd.Height;
    int stride = bd.Stride;
    BYTE* dst0 = (BYTE*)bd.Scan0;

    size_t bufSize = (size_t)h * (size_t)stride;
    std::vector<BYTE> src(bufSize);
    memcpy(src.data(), dst0, bufSize);

    auto dp = [&](int x, int y)->BYTE* { return dst0 + (size_t)y * (size_t)stride + (size_t)x * 4; };

    int passes = iclamp(strength, 1, 3);

    for (int pass = 0; pass < passes; pass++)
    {
        std::vector<BYTE> src2(bufSize);
        memcpy(src2.data(), dst0, bufSize);
        auto sp2 = [&](int x, int y)->BYTE* { return src2.data() + (size_t)y * (size_t)stride + (size_t)x * 4; };

        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int lum[9];
                int idx = 0;
                for (int oy = -1; oy <= 1; oy++)
                {
                    int yy = iclamp(y + oy, 0, h - 1);
                    for (int ox = -1; ox <= 1; ox++)
                    {
                        int xx = iclamp(x + ox, 0, w - 1);
                        BYTE* p = sp2(xx, yy);
                        int l = (p[2] * 77 + p[1] * 150 + p[0] * 29) >> 8;
                        lum[idx++] = l;
                    }
                }
                std::sort(lum, lum + 9);
                int m = lum[4];

                BYTE* o = dp(x, y);
                int l0 = (o[2] * 77 + o[1] * 150 + o[0] * 29) >> 8;
                int d = m - l0;

                o[0] = (BYTE)iclamp(o[0] + d, 0, 255);
                o[1] = (BYTE)iclamp(o[1] + d, 0, 255);
                o[2] = (BYTE)iclamp(o[2] + d, 0, 255);
            }
    }

    bmp->UnlockBits(&bd);
}

static void MildBlur(Bitmap* bmp, int radius)
{
    if (!bmp || bmp->GetLastStatus() != Ok) return;

    BitmapData bd{};
    Lock32(bmp, bd);
    if (!bd.Scan0 || bd.Width <= 0 || bd.Height <= 0 || bd.Stride == 0)
    {
        bmp->UnlockBits(&bd);
        return;
    }

    int w = bd.Width;
    int h = bd.Height;
    int stride = bd.Stride;
    BYTE* dst0 = (BYTE*)bd.Scan0;

    size_t bufSize = (size_t)h * (size_t)stride;
    std::vector<BYTE> src(bufSize);
    memcpy(src.data(), dst0, bufSize);

    auto sp = [&](int x, int y)->BYTE* { return src.data() + (size_t)y * (size_t)stride + (size_t)x * 4; };
    auto dp = [&](int x, int y)->BYTE* { return dst0 + (size_t)y * (size_t)stride + (size_t)x * 4; };

    int r = iclamp(radius, 1, 3);

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            int sumB = 0, sumG = 0, sumR = 0, cnt = 0;
            for (int oy = -r; oy <= r; oy++)
            {
                int yy = iclamp(y + oy, 0, h - 1);
                for (int ox = -r; ox <= r; ox++)
                {
                    int xx = iclamp(x + ox, 0, w - 1);
                    BYTE* p = sp(xx, yy);
                    sumB += p[0]; sumG += p[1]; sumR += p[2];
                    cnt++;
                }
            }
            BYTE* o = dp(x, y);
            o[0] = (BYTE)(sumB / cnt);
            o[1] = (BYTE)(sumG / cnt);
            o[2] = (BYTE)(sumR / cnt);
        }

    bmp->UnlockBits(&bd);
}

void ApplyDenoise(Bitmap* bmp, DenoiseMode mode)
{
    if (!bmp) return;

    if (mode == DenoiseMode::Manga)
    {
        Median3x3Luma(bmp, 2);
        UnsharpMask32(bmp, 0.95f, 1);
    }
    else if (mode == DenoiseMode::Color)
    {
        MildBlur(bmp, 1);
        UnsharpMask32(bmp, 0.70f, 1);
    }
    else
    {
        Median3x3Luma(bmp, 1);
        UnsharpMask32(bmp, 0.80f, 1);
    }
}

std::unique_ptr<Bitmap> BuildThumb(const std::wstring& path, bool allowImage)
{
    std::unique_ptr<Bitmap> thumb(new Bitmap(CELL, CELL, PixelFormat32bppARGB));
    if (!thumb || thumb->GetLastStatus() != Ok) return nullptr;

    Graphics gg(thumb.get());
    gg.SetSmoothingMode(SmoothingModeHighQuality);
    gg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    gg.Clear(Color(255, 18, 18, 18));

    Pen br(Color(255, 45, 45, 45), 1.0f);

    if (allowImage && IsImageFile(path))
    {
        std::unique_ptr<Bitmap> src(new Bitmap(path.c_str()));
        if (src && src->GetLastStatus() == Ok)
        {
            int iw = (int)src->GetWidth();
            int ih = (int)src->GetHeight();
            if (iw > 0 && ih > 0)
            {
                float s = min((float)CELL / (float)iw, (float)CELL / (float)ih);
                int dw = (int)(iw * s);
                int dh = (int)(ih * s);
                int dx = (CELL - dw) / 2;
                int dy = (CELL - dh) / 2;
                Rect dst(dx, dy, dw, dh);
                gg.DrawImage(src.get(), dst, 0, 0, iw, ih, UnitPixel);
                gg.DrawRectangle(&br, Rect(0, 0, CELL - 1, CELL - 1));
                return thumb;
            }
        }
    }

    SolidBrush ph(Color(255, 22, 22, 22));
    gg.FillRectangle(&ph, 0, 0, CELL, CELL);
    gg.DrawRectangle(&br, 0, 0, CELL - 1, CELL - 1);

    std::wstring ext = GetExtLower(path);
    if (ext.empty()) ext = L"file";

    extern void DrawTextG(Graphics & g, const std::wstring & text, float x, float y, float w, float h, float size, Color color, bool bold, int alignH);
    DrawTextG(gg, ext, 0.f, 0.f, (float)CELL, (float)CELL, 18.f, C_SUB, true, 0);
    return thumb;
}

void EnsureThumb(size_t i)
{
    if (i >= g_thumbs.size()) return;
    if (g_thumbs[i]) return;
    bool allowImage = (g_tool != Tool::Rename);
    g_thumbs[i] = BuildThumb(g_inputs[i], allowImage);
}
