#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <algorithm>
#include <memory>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "Shell32.lib")

using namespace Gdiplus;

constexpr wchar_t WINDOW_CLASS[] = L"MangaDenoiserWindow";
constexpr wchar_t WINDOW_TITLE[] = L"Manga Denoiser";

constexpr int TOOLBAR_H = 56;
constexpr int FOOTER_H = 44;

constexpr int PAD = 16;
constexpr int CELL = 140;
constexpr int GAP = 16;

constexpr int BTN_H = 32;

constexpr UINT_PTR TIMER_SCROLL = 1;
constexpr UINT_PTR TIMER_PROCESS = 2;
constexpr UINT_PTR TIMER_DONE_ANIM = 3;

struct RectI { int x, y, w, h; };
static inline bool PtIn(const RectI& r, int px, int py) { return px >= r.x && px < (r.x + r.w) && py >= r.y && py < (r.y + r.h); }
static inline int ClampI(int v, int a, int b) { return (v < a) ? a : (v > b) ? b : v; }

enum class Screen { Gallery, Processing, Done };
enum class DenoiseMode { MangaBW_Sharp, Color_Clean, Balanced };

static Color C_BG(255, 24, 24, 24);
static Color C_PANEL(255, 32, 32, 32);
static Color C_BTN(255, 55, 55, 55);
static Color C_BTN_DIS(255, 40, 40, 40);
static Color C_BORDER(255, 90, 90, 90);
static Color C_TEXT(255, 220, 220, 220);
static Color C_SUB(255, 170, 170, 170);
static Color C_ACC(255, 255, 120, 205);

static ULONG_PTR g_gdiplusToken = 0;

static Screen g_screen = Screen::Gallery;
static DenoiseMode g_mode = DenoiseMode::MangaBW_Sharp;

static std::vector<std::wstring> g_images;
static std::vector<std::unique_ptr<Bitmap>> g_thumbs;

static std::wstring g_inputFolder;
static std::wstring g_outputFolder;

static std::wstring g_status = L"Ready";

static int g_scrollY = 0;
static int g_scrollTarget = 0;
static int g_scrollMax = 0;

static bool g_processing = false;
static int  g_processed = 0;

static int g_targetWidth = 1200;

static int g_doneTick = 0;

static LARGE_INTEGER g_qpf{};
static LARGE_INTEGER g_procStart{};
static double g_totalSeconds = 0.0;

static HBITMAP g_backBmp = nullptr;
static HDC g_backDC = nullptr;
static int g_backW = 0;
static int g_backH = 0;

static bool IsImageFile(const std::wstring& p)
{
    auto dot = p.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = p.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext == L"png" || ext == L"jpg" || ext == L"jpeg" || ext == L"bmp" || ext == L"webp";
}

static void AddImagesFromFolder(const std::wstring& folder, std::vector<std::wstring>& out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\*.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring f = folder + L"\\" + fd.cFileName;
        if (IsImageFile(f)) out.push_back(f);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

static std::wstring PickFolderDialog(HWND hWnd, const wchar_t* title)
{
    BROWSEINFOW bi{};
    bi.hwndOwner = hWnd;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return L"";

    wchar_t path[MAX_PATH]{};
    if (!SHGetPathFromIDListW(pidl, path)) {
        CoTaskMemFree(pidl);
        return L"";
    }

    CoTaskMemFree(pidl);
    return path;
}

static std::wstring EllipsizePath(const std::wstring& s, int maxChars)
{
    if ((int)s.size() <= maxChars) return s;
    if (maxChars < 10) return s.substr(0, maxChars);
    int keepL = maxChars / 2 - 2;
    int keepR = maxChars - keepL - 3;
    return s.substr(0, keepL) + L"..." + s.substr((int)s.size() - keepR);
}

static int ComputeCols(int clientW)
{
    int usable = clientW - PAD * 2;
    int step = CELL + GAP;
    return max(1, usable / step);
}

static int ComputeContentHeight(int cols)
{
    if (g_images.empty()) return PAD * 2;
    int rows = (int)((g_images.size() + cols - 1) / cols);
    int h = PAD * 2 + rows * (CELL + GAP) - GAP;
    return max(h, PAD * 2);
}

static void ResetScroll()
{
    g_scrollY = 0;
    g_scrollTarget = 0;
    g_scrollMax = 0;
}

static void ResetToInitial(HWND hWnd)
{
    g_screen = Screen::Gallery;
    g_mode = DenoiseMode::MangaBW_Sharp;

    g_images.clear();
    g_thumbs.clear();

    g_inputFolder.clear();
    g_outputFolder.clear();

    g_processing = false;
    g_processed = 0;

    g_targetWidth = 1200;

    g_status = L"Ready";
    g_totalSeconds = 0.0;

    ResetScroll();

    KillTimer(hWnd, TIMER_PROCESS);
    KillTimer(hWnd, TIMER_DONE_ANIM);

    InvalidateRect(hWnd, nullptr, TRUE);
}

static void EnsureBackbuffer(HDC hdc, int w, int h)
{
    if (g_backDC && g_backBmp && w == g_backW && h == g_backH) return;

    if (g_backDC) { DeleteDC(g_backDC); g_backDC = nullptr; }
    if (g_backBmp) { DeleteObject(g_backBmp); g_backBmp = nullptr; }

    g_backDC = CreateCompatibleDC(hdc);
    g_backBmp = CreateCompatibleBitmap(hdc, w, h);
    SelectObject(g_backDC, g_backBmp);

    g_backW = w;
    g_backH = h;
}

static void DrawTextG(Graphics& g, const std::wstring& text, float x, float y, float w, float h, float size, Color color, bool bold, int align)
{
    FontFamily ff(L"Segoe UI");
    Font f(&ff, size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    SolidBrush b(color);

    RectF r(x, y, w, h);
    StringFormat sf;
    sf.SetLineAlignment(StringAlignmentCenter);
    if (align < 0) sf.SetAlignment(StringAlignmentNear);
    else if (align > 0) sf.SetAlignment(StringAlignmentFar);
    else sf.SetAlignment(StringAlignmentCenter);

    g.DrawString(text.c_str(), -1, &f, r, &sf, &b);
}

static void BuildRoundRectPath(GraphicsPath& path, float x, float y, float w, float h, float r)
{
    float d = r * 2.0f;
    path.Reset();
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

static void FillRoundRect(Graphics& g, Brush& br, float x, float y, float w, float h, float r)
{
    GraphicsPath p;
    BuildRoundRectPath(p, x, y, w, h, r);
    g.FillPath(&br, &p);
}

static void DrawRoundRect(Graphics& g, Pen& pen, float x, float y, float w, float h, float r)
{
    GraphicsPath p;
    BuildRoundRectPath(p, x, y, w, h, r);
    g.DrawPath(&pen, &p);
}

static void DrawButton(Graphics& g, const RectI& r, const wchar_t* text, bool disabled, bool accentLine = true)
{
    SolidBrush bg(disabled ? C_BTN_DIS : C_BTN);
    Pen br(C_BORDER, 1.0f);

    g.FillRectangle(&bg, r.x, r.y, r.w, r.h);
    g.DrawRectangle(&br, r.x, r.y, r.w, r.h);

    if (!disabled && accentLine) {
        Pen acc(C_ACC, 2.0f);
        g.DrawLine(&acc, r.x + 1, r.y + r.h - 1, r.x + r.w - 1, r.y + r.h - 1);
    }

    DrawTextG(g, text, (float)r.x, (float)r.y, (float)r.w, (float)r.h, 14.0f,
        disabled ? Color(255, 130, 130, 130) : C_TEXT, false, 0);
}

static void DrawPanelOutline(Graphics& g, int w, int h)
{
    Pen acc(C_ACC, 2.0f);
    g.DrawRectangle(&acc, 1, 1, w - 3, h - 3);
}

static std::unique_ptr<Bitmap> BuildThumb(const std::wstring& path)
{
    std::unique_ptr<Bitmap> src(new Bitmap(path.c_str()));
    if (!src || src->GetLastStatus() != Ok) return nullptr;

    std::unique_ptr<Bitmap> thumb(new Bitmap(CELL, CELL, PixelFormat32bppARGB));
    if (!thumb || thumb->GetLastStatus() != Ok) return nullptr;

    Graphics gg(thumb.get());
    gg.SetSmoothingMode(SmoothingModeHighQuality);
    gg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    gg.Clear(Color(255, 18, 18, 18));

    const int iw = (int)src->GetWidth();
    const int ih = (int)src->GetHeight();
    if (iw <= 0 || ih <= 0) return thumb;

    float s = min((float)CELL / (float)iw, (float)CELL / (float)ih);
    int dw = (int)(iw * s);
    int dh = (int)(ih * s);
    int dx = (CELL - dw) / 2;
    int dy = (CELL - dh) / 2;

    Rect dst(dx, dy, dw, dh);
    gg.DrawImage(src.get(), dst, 0, 0, iw, ih, UnitPixel);

    Pen p(Color(255, 45, 45, 45), 1.0f);
    gg.DrawRectangle(&p, Rect(0, 0, CELL - 1, CELL - 1));

    return thumb;
}

static void EnsureThumb(size_t i)
{
    if (i >= g_thumbs.size()) return;
    if (g_thumbs[i]) return;
    g_thumbs[i] = BuildThumb(g_images[i]);
}

static void ComputeScroll(const RECT& rc)
{
    if (g_screen != Screen::Gallery) {
        ResetScroll();
        return;
    }

    int viewH = (rc.bottom - rc.top) - TOOLBAR_H - FOOTER_H;
    int cols = ComputeCols(rc.right);
    int contentH = ComputeContentHeight(cols);

    g_scrollMax = max(0, contentH - viewH);
    g_scrollTarget = ClampI(g_scrollTarget, 0, g_scrollMax);
    g_scrollY = ClampI(g_scrollY, 0, g_scrollMax);
}

static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;

    std::unique_ptr<BYTE[]> mem(new BYTE[size]);
    ImageCodecInfo* pImageCodecInfo = (ImageCodecInfo*)mem.get();
    GetImageEncoders(num, size, pImageCodecInfo);

    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            return (int)j;
        }
    }
    return -1;
}

static std::unique_ptr<Bitmap> CloneTo32bpp(Bitmap* src)
{
    if (!src || src->GetLastStatus() != Ok) return nullptr;

    int w = (int)src->GetWidth();
    int h = (int)src->GetHeight();
    if (w <= 0 || h <= 0) return nullptr;

    std::unique_ptr<Bitmap> out(new Bitmap(w, h, PixelFormat32bppARGB));
    if (!out || out->GetLastStatus() != Ok) return nullptr;

    Graphics g(out.get());
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.DrawImage(src, 0, 0, w, h);

    return out;
}

static std::unique_ptr<Bitmap> ResizeToWidth(Bitmap* src, int targetW)
{
    if (!src || src->GetLastStatus() != Ok) return nullptr;
    int sw = (int)src->GetWidth();
    int sh = (int)src->GetHeight();
    if (sw <= 0 || sh <= 0) return nullptr;

    if (targetW <= 0 || targetW == sw) return CloneTo32bpp(src);

    int tw = targetW;
    int th = (int)std::lround((double)sh * (double)tw / (double)sw);
    th = max(1, th);

    std::unique_ptr<Bitmap> out(new Bitmap(tw, th, PixelFormat32bppARGB));
    if (!out || out->GetLastStatus() != Ok) return nullptr;

    Graphics g(out.get());
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetSmoothingMode(SmoothingModeHighQuality);
    g.DrawImage(src, 0, 0, tw, th);

    return out;
}

static inline int LumaI(int r, int g, int b)
{
    return (77 * r + 150 * g + 29 * b) >> 8;
}

static void BoxBlurLuma(const std::vector<int>& inY, std::vector<int>& outY, int w, int h, int radius)
{
    outY.assign(w * h, 0);
    if (radius <= 0) { outY = inY; return; }

    int dia = radius * 2 + 1;
    int area = dia * dia;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sum = 0;
            int y0 = max(0, y - radius);
            int y1 = min(h - 1, y + radius);
            int x0 = max(0, x - radius);
            int x1 = min(w - 1, x + radius);

            for (int yy = y0; yy <= y1; yy++) {
                const int* row = &inY[yy * w];
                for (int xx = x0; xx <= x1; xx++) sum += row[xx];
            }

            int count = (y1 - y0 + 1) * (x1 - x0 + 1);
            outY[y * w + x] = sum / max(1, count);
        }
    }
}

static void UnsharpLuma(std::vector<int>& Y, int w, int h, int radius, float amount, int threshold)
{
    std::vector<int> blur;
    BoxBlurLuma(Y, blur, w, h, radius);

    for (int i = 0; i < w * h; i++) {
        int hp = Y[i] - blur[i];
        if (abs(hp) < threshold) hp = 0;
        int v = (int)std::lround((float)Y[i] + amount * (float)hp);
        Y[i] = ClampI(v, 0, 255);
    }
}

static void Bilateral1D(const std::vector<int>& src, std::vector<int>& dst, int w, int h, int radius, int sigmaRange)
{
    dst.assign(w * h, 0);
    if (radius <= 0 || sigmaRange <= 0) { dst = src; return; }

    const float inv2sr2 = 1.0f / (2.0f * (float)sigmaRange * (float)sigmaRange);

    static float spatialW[9][9]{};
    static bool spatialInit = false;
    if (!spatialInit) {
        for (int dy = -4; dy <= 4; dy++) {
            for (int dx = -4; dx <= 4; dx++) {
                float d2 = (float)(dx * dx + dy * dy);
                spatialW[dy + 4][dx + 4] = std::exp(-d2 / (2.0f * 2.0f * 2.0f));
            }
        }
        spatialInit = true;
    }

    int r = ClampI(radius, 1, 4);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int c = src[y * w + x];

            float sumW = 0.0f;
            float sumV = 0.0f;

            int y0 = max(0, y - r);
            int y1 = min(h - 1, y + r);
            int x0 = max(0, x - r);
            int x1 = min(w - 1, x + r);

            for (int yy = y0; yy <= y1; yy++) {
                for (int xx = x0; xx <= x1; xx++) {
                    int v = src[yy * w + xx];
                    int dv = v - c;
                    float rw = std::exp(-(float)(dv * dv) * inv2sr2);
                    float sw = spatialW[(yy - y) + 4][(xx - x) + 4];
                    float wgt = rw * sw;
                    sumW += wgt;
                    sumV += wgt * (float)v;
                }
            }

            int out = (int)std::lround(sumV / max(1e-6f, sumW));
            dst[y * w + x] = out;
        }
    }
}

static void ComputeEdgeMask(const std::vector<int>& Y, std::vector<float>& edge, int w, int h, int t0, int t1, float power)
{
    edge.assign(w * h, 0.0f);
    float inv = 1.0f / (float)max(1, (t1 - t0));

    for (int y = 1; y < h - 1; y++) {
        for (int x = 1; x < w - 1; x++) {
            int gx =
                -Y[(y - 1) * w + (x - 1)] + Y[(y - 1) * w + (x + 1)] +
                -2 * Y[y * w + (x - 1)] + 2 * Y[y * w + (x + 1)] +
                -Y[(y + 1) * w + (x - 1)] + Y[(y + 1) * w + (x + 1)];

            int gy =
                -Y[(y - 1) * w + (x - 1)] - 2 * Y[(y - 1) * w + x] - Y[(y - 1) * w + (x + 1)] +
                Y[(y + 1) * w + (x - 1)] + 2 * Y[(y + 1) * w + x] + Y[(y + 1) * w + (x + 1)];

            int m = (int)std::lround(std::sqrt((double)gx * gx + (double)gy * gy) / 4.0);
            float e = (float)(m - t0) * inv;
            if (e < 0.0f) e = 0.0f;
            if (e > 1.0f) e = 1.0f;
            edge[y * w + x] = std::pow(e, power);
        }
    }
}

static std::unique_ptr<Bitmap> DenoiseAndSharpen(Bitmap* bmp, DenoiseMode mode)
{
    if (!bmp || bmp->GetLastStatus() != Ok) return nullptr;

    int w = (int)bmp->GetWidth();
    int h = (int)bmp->GetHeight();
    if (w <= 0 || h <= 0) return nullptr;

    Rect rect(0, 0, w, h);
    BitmapData bd{};
    if (bmp->LockBits(&rect, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Ok) return nullptr;

    auto* p = (BYTE*)bd.Scan0;
    int stride = bd.Stride;

    std::vector<int> Y(w * h), Cb(w * h), Cr(w * h);

    for (int y = 0; y < h; y++) {
        BYTE* row = p + y * stride;
        for (int x = 0; x < w; x++) {
            BYTE b = row[x * 4 + 0];
            BYTE g = row[x * 4 + 1];
            BYTE r = row[x * 4 + 2];
            int yy = LumaI(r, g, b);
            Y[y * w + x] = yy;
            Cb[y * w + x] = (int)b - yy;
            Cr[y * w + x] = (int)r - yy;
        }
    }

    int rBilateral = 2;
    int sigmaY = 18;
    int sigmaC = 26;
    int t0 = 18, t1 = 90;
    float epow = 0.72f;

    int usRadius = 1;
    float usAmount = 0.75f;
    int usThresh = 4;

    if (mode == DenoiseMode::MangaBW_Sharp) {
        rBilateral = 2;
        sigmaY = 15;
        sigmaC = 10;
        t0 = 22; t1 = 105;
        epow = 0.80f;
        usRadius = 1;
        usAmount = 1.05f;
        usThresh = 3;
    }
    else if (mode == DenoiseMode::Color_Clean) {
        rBilateral = 2;
        sigmaY = 18;
        sigmaC = 34;
        t0 = 18; t1 = 85;
        epow = 0.65f;
        usRadius = 1;
        usAmount = 0.65f;
        usThresh = 6;
    }
    else {
        rBilateral = 2;
        sigmaY = 18;
        sigmaC = 26;
        t0 = 18; t1 = 95;
        epow = 0.72f;
        usRadius = 1;
        usAmount = 0.80f;
        usThresh = 4;
    }

    std::vector<int> Yf, Cbf, Crf;
    Bilateral1D(Y, Yf, w, h, rBilateral, sigmaY);

    if (mode == DenoiseMode::Color_Clean) {
        Bilateral1D(Cb, Cbf, w, h, rBilateral, sigmaC);
        Bilateral1D(Cr, Crf, w, h, rBilateral, sigmaC);
    }
    else {
        Cbf = Cb;
        Crf = Cr;
    }

    std::vector<float> edge;
    ComputeEdgeMask(Y, edge, w, h, t0, t1, epow);

    for (int i = 0; i < w * h; i++) {
        float e = edge[i];
        float keep = e;
        int v = (int)std::lround((1.0f - keep) * (float)Yf[i] + keep * (float)Y[i]);
        Y[i] = ClampI(v, 0, 255);
    }

    UnsharpLuma(Y, w, h, usRadius, usAmount, usThresh);

    for (int y = 0; y < h; y++) {
        BYTE* row = p + y * stride;
        for (int x = 0; x < w; x++) {
            int i = y * w + x;
            int yy = Y[i];
            int cb = Cbf[i];
            int cr = Crf[i];

            int rr = ClampI(yy + cr, 0, 255);
            int bb = ClampI(yy + cb, 0, 255);
            int gg = ClampI((yy * 256 - 77 * rr - 29 * bb) / 150, 0, 255);

            row[x * 4 + 2] = (BYTE)rr;
            row[x * 4 + 1] = (BYTE)gg;
            row[x * 4 + 0] = (BYTE)bb;
        }
    }

    bmp->UnlockBits(&bd);

    std::unique_ptr<Bitmap> out(new Bitmap(w, h, PixelFormat32bppARGB));
    Graphics gr(out.get());
    gr.DrawImage(bmp, 0, 0, w, h);
    return out;
}

static std::wstring FileNameOnly(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return path;
    return path.substr(p + 1);
}

static std::wstring FileNameNoExt(const std::wstring& name)
{
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return name;
    return name.substr(0, dot);
}

static bool SavePng(Bitmap* bmp, const std::wstring& outPath)
{
    CLSID clsid{};
    if (GetEncoderClsid(L"image/png", &clsid) < 0) return false;
    return bmp->Save(outPath.c_str(), &clsid, nullptr) == Ok;
}

static bool ProcessOne(HWND hWnd)
{
    if (g_processed < 0 || g_processed >= (int)g_images.size()) return false;

    std::wstring inPath = g_images[(size_t)g_processed];
    Bitmap src(inPath.c_str());
    if (src.GetLastStatus() != Ok) {
        g_processed++;
        g_status = L"Skipped: failed to load";
        return true;
    }

    auto resized = ResizeToWidth(&src, g_targetWidth);
    if (!resized || resized->GetLastStatus() != Ok) {
        g_processed++;
        g_status = L"Skipped: resize failed";
        return true;
    }

    auto processed = DenoiseAndSharpen(resized.get(), g_mode);
    if (!processed || processed->GetLastStatus() != Ok) {
        g_processed++;
        g_status = L"Skipped: denoise failed";
        return true;
    }

    std::wstring base = FileNameNoExt(FileNameOnly(inPath));
    std::wstring outPath = g_outputFolder + L"\\" + base + L"_denoised.png";

    if (!SavePng(processed.get(), outPath)) {
        g_processed++;
        g_status = L"Save failed";
        return true;
    }

    g_processed++;
    g_status = L"Processing...";
    return true;
}

static void StartProcessing(HWND hWnd)
{
    if (g_images.empty()) return;
    if (g_outputFolder.empty()) return;

    g_processing = true;
    g_processed = 0;
    g_status = L"Processing...";

    QueryPerformanceCounter(&g_procStart);

    KillTimer(hWnd, TIMER_PROCESS);
    SetTimer(hWnd, TIMER_PROCESS, 1, nullptr);
}

static void StopProcessing(HWND hWnd)
{
    g_processing = false;
    KillTimer(hWnd, TIMER_PROCESS);
}

static void SwitchToDone(HWND hWnd)
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    LONGLONG dt = now.QuadPart - g_procStart.QuadPart;
    g_totalSeconds = (double)dt / (double)g_qpf.QuadPart;

    g_screen = Screen::Done;
    g_doneTick = 0;
    g_status = L"Done";

    KillTimer(hWnd, TIMER_DONE_ANIM);
    SetTimer(hWnd, TIMER_DONE_ANIM, 50, nullptr);
}

static void LoadFromDropped(HDROP hDrop)
{
    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    wchar_t path[MAX_PATH]{};

    std::vector<std::wstring> collected;
    std::wstring detectedFolder;

    for (UINT i = 0; i < count; i++) {
        if (!DragQueryFileW(hDrop, i, path, MAX_PATH)) continue;
        std::wstring p = path;

        DWORD attr = GetFileAttributesW(p.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;

        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            detectedFolder = p;
            AddImagesFromFolder(p, collected);
        }
        else {
            if (IsImageFile(p)) collected.push_back(p);
        }
    }

    g_images = std::move(collected);
    std::sort(g_images.begin(), g_images.end());
    g_thumbs.clear();
    g_thumbs.resize(g_images.size());

    g_inputFolder = detectedFolder;
    ResetScroll();

    g_outputFolder.clear();
    g_processing = false;
    g_processed = 0;
    g_totalSeconds = 0.0;

    g_status = g_images.empty() ? L"No images found" : L"Images loaded";
}

static RectI ToolbarBtnAdd(const RECT&) { return RectI{ PAD, 12, 160, BTN_H }; }
static RectI ToolbarBtnNext(const RECT&) { return RectI{ PAD + 160 + 12, 12, 140, BTN_H }; }

static RectI FooterBtnBack(const RECT& rc) { return RectI{ PAD, rc.bottom - FOOTER_H + 6, 110, 32 }; }
static RectI FooterBtnRight(const RECT& rc) { return RectI{ rc.right - PAD - 180, rc.bottom - FOOTER_H + 6, 180, 32 }; }

static RectI ProcOutPick(const RECT&) { return RectI{ PAD, TOOLBAR_H + 110, 420, 40 }; }
static RectI ProcStart(const RECT&) { return RectI{ PAD, TOOLBAR_H + 170, 180, 36 }; }

static RectI ModeBtn1(const RECT&) { return RectI{ PAD, TOOLBAR_H + 56, 170, 32 }; }
static RectI ModeBtn2(const RECT&) { return RectI{ PAD + 170 + 10, TOOLBAR_H + 56, 170, 32 }; }
static RectI ModeBtn3(const RECT&) { return RectI{ PAD + (170 + 10) * 2, TOOLBAR_H + 56, 170, 32 }; }

static RectI WidthMinus(const RECT&) { return RectI{ PAD, TOOLBAR_H + 230, 40, 32 }; }
static RectI WidthBox(const RECT&) { return RectI{ PAD + 40 + 10, TOOLBAR_H + 230, 140, 32 }; }
static RectI WidthPlus(const RECT&) { return RectI{ PAD + 40 + 10 + 140 + 10, TOOLBAR_H + 230, 40, 32 }; }

static void DrawCat(Graphics& g, int cx, int cy, int tick)
{
    float t = (float)tick * 0.09f;
    float sway = std::sin(t) * 14.0f;

    SolidBrush body(Color(255, 50, 50, 50));
    SolidBrush face(Color(255, 62, 62, 62));
    SolidBrush eye(Color(255, 235, 235, 235));
    SolidBrush blush(C_ACC);
    Pen outline(Color(255, 95, 95, 95), 2.0f);
    Pen whisk(C_SUB, 2.0f);
    Pen acc(C_ACC, 2.0f);

    RectF head((REAL)(cx - 120), (REAL)(cy - 120), 240.0f, 200.0f);
    FillRoundRect(g, face, head.X, head.Y, head.Width, head.Height, 36.0f);
    DrawRoundRect(g, outline, head.X, head.Y, head.Width, head.Height, 36.0f);

    Point earL[3] = { Point(cx - 85, cy - 120), Point(cx - 130, cy - 172), Point(cx - 35, cy - 150) };
    Point earR[3] = { Point(cx + 85, cy - 120), Point(cx + 130, cy - 172), Point(cx + 35, cy - 150) };
    g.FillPolygon(&face, earL, 3);
    g.FillPolygon(&face, earR, 3);
    g.DrawPolygon(&outline, earL, 3);
    g.DrawPolygon(&outline, earR, 3);

    g.FillEllipse(&eye, cx - 55, cy - 40, 22, 22);
    g.FillEllipse(&eye, cx + 33, cy - 40, 22, 22);

    g.FillEllipse(&blush, cx - 85, cy - 10, 26, 14);
    g.FillEllipse(&blush, cx + 59, cy - 10, 26, 14);

    g.DrawLine(&outline, cx, cy - 10, cx, cy + 12);
    g.DrawArc(&outline, cx - 18, cy + 6, 18, 14, 0, 180);
    g.DrawArc(&outline, cx, cy + 6, 18, 14, 0, 180);

    g.DrawLine(&whisk, cx - 45, cy - 10, cx - 120, cy - 25);
    g.DrawLine(&whisk, cx - 45, cy, cx - 120, cy);
    g.DrawLine(&whisk, cx - 45, cy + 10, cx - 120, cy + 25);

    g.DrawLine(&whisk, cx + 45, cy - 10, cx + 120, cy - 25);
    g.DrawLine(&whisk, cx + 45, cy, cx + 120, cy);
    g.DrawLine(&whisk, cx + 45, cy + 10, cx + 120, cy + 25);

    int bw = 210, bh = 150;
    RectF bodyR((REAL)(cx - bw / 2), (REAL)(cy + 55), (REAL)bw, (REAL)bh);
    FillRoundRect(g, body, bodyR.X, bodyR.Y, bodyR.Width, bodyR.Height, 22.0f);
    DrawRoundRect(g, outline, bodyR.X, bodyR.Y, bodyR.Width, bodyR.Height, 22.0f);

    float baseX = (float)(cx + bw / 2 - 10);
    float baseY = (float)(cy + 135);
    float len = 120.0f;

    PointF p0(baseX, baseY);
    PointF p1(baseX + 30.0f + sway, baseY - 20.0f);
    PointF p2(baseX + 65.0f + sway * 0.8f, baseY - 55.0f);
    PointF p3(baseX + 95.0f + sway * 0.6f, baseY - 85.0f);

    GraphicsPath tail;
    tail.AddBezier(p0, p1, p2, p3);
    g.DrawPath(&acc, &tail);
}

static void SetMode(DenoiseMode m)
{
    g_mode = m;
    if (m == DenoiseMode::MangaBW_Sharp) g_status = L"Mode: Manga BW";
    else if (m == DenoiseMode::Color_Clean) g_status = L"Mode: Color";
    else g_status = L"Mode: Balanced";
}

static std::wstring ModeName()
{
    if (g_mode == DenoiseMode::MangaBW_Sharp) return L"Manga BW (Sharp)";
    if (g_mode == DenoiseMode::Color_Clean) return L"Color (Clean)";
    return L"Balanced";
}

static void DrawScreen(Graphics& g, const RECT& rc)
{
    g.SetSmoothingMode(SmoothingModeHighQuality);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.Clear(C_BG);

    SolidBrush toolbar(C_PANEL);
    g.FillRectangle(&toolbar, 0, 0, rc.right, TOOLBAR_H);

    Pen acc(C_ACC, 2.0f);
    g.DrawLine(&acc, 0, TOOLBAR_H - 1, rc.right, TOOLBAR_H - 1);

    RectI btnAdd = ToolbarBtnAdd(rc);
    RectI btnNext = ToolbarBtnNext(rc);

    bool nextDisabled = g_images.empty();
    DrawButton(g, btnAdd, L"Add folder", false);
    DrawButton(g, btnNext, L"Next", nextDisabled);

    std::wstring folderLine = g_inputFolder.empty() ? L"Drop images/folder to start" : g_inputFolder;
    folderLine = EllipsizePath(folderLine, 90);
    DrawTextG(g, folderLine, (float)(PAD + btnAdd.w + 12 + btnNext.w + 12), 0.f,
        (float)(rc.right - PAD - (PAD + btnAdd.w + 12 + btnNext.w + 12)), (float)TOOLBAR_H,
        13.0f, C_SUB, false, -1);

    SolidBrush footer(C_PANEL);
    g.FillRectangle(&footer, 0, rc.bottom - FOOTER_H, rc.right, FOOTER_H);
    g.DrawLine(&acc, 0, rc.bottom - FOOTER_H, rc.right, rc.bottom - FOOTER_H);

    RectI btnBack = FooterBtnBack(rc);
    RectI btnRight = FooterBtnRight(rc);

    bool showBack = (g_screen != Screen::Gallery);
    if (showBack) DrawButton(g, btnBack, L"Back", false);

    if (g_screen == Screen::Gallery) {
        DrawButton(g, btnRight, L"Next", g_images.empty());
    }
    else if (g_screen == Screen::Processing) {
        bool canStart = !g_outputFolder.empty() && !g_images.empty() && !g_processing;
        DrawButton(g, btnRight, g_processing ? L"Processing..." : L"Start", !canStart);
    }
    else {
        DrawButton(g, btnRight, L"Process more", false);
    }

    int statusX = PAD;
    if (showBack) statusX = btnBack.x + btnBack.w + 16;

    std::wstring footerTxt = L"Images: " + std::to_wstring(g_images.size()) +
        L"    Mode: " + ModeName() +
        L"    Status: " + g_status;

    DrawTextG(g, footerTxt, (float)statusX, (float)(rc.bottom - FOOTER_H),
        (float)(btnRight.x - statusX - 12), (float)FOOTER_H, 13.0f, C_SUB, false, -1);

    DrawPanelOutline(g, rc.right, rc.bottom);

    if (g_screen == Screen::Gallery)
    {
        int viewTop = TOOLBAR_H;
        int viewBottom = rc.bottom - FOOTER_H;
        int viewH = viewBottom - viewTop;

        int cols = ComputeCols(rc.right);
        int contentH = ComputeContentHeight(cols);

        g_scrollMax = max(0, contentH - viewH);
        g_scrollTarget = ClampI(g_scrollTarget, 0, g_scrollMax);
        g_scrollY = ClampI(g_scrollY, 0, g_scrollMax);

        Rect clipR(0, viewTop, rc.right, viewH);
        g.SetClip(clipR);

        if (g_images.empty())
        {
            DrawTextG(g, L"Drop images or a folder here", 0.f, (float)viewTop, (float)rc.right, (float)viewH, 22.f, C_TEXT, true, 0);
        }
        else
        {
            int step = CELL + GAP;
            int y0 = viewTop + PAD - g_scrollY;

            int firstRow = max(0, (g_scrollY - PAD) / step);
            int rowsVisible = (viewH / step) + 3;
            int totalRows = (int)((g_images.size() + cols - 1) / cols);
            int lastRow = min(totalRows - 1, firstRow + rowsVisible);

            for (int row = firstRow; row <= lastRow; row++) {
                for (int col = 0; col < cols; col++) {
                    size_t i = (size_t)row * (size_t)cols + (size_t)col;
                    if (i >= g_images.size()) break;

                    int x = PAD + col * step;
                    int y = y0 + row * step;

                    EnsureThumb(i);
                    Rect r(x, y, CELL, CELL);

                    if (g_thumbs[i] && g_thumbs[i]->GetLastStatus() == Ok) {
                        g.DrawImage(g_thumbs[i].get(), r);
                    }
                    else {
                        SolidBrush ph(Color(255, 18, 18, 18));
                        Pen br(Color(255, 45, 45, 45), 1.0f);
                        g.FillRectangle(&ph, r);
                        g.DrawRectangle(&br, r);
                    }
                }
            }
        }

        g.ResetClip();
    }
    else if (g_screen == Screen::Processing)
    {
        DrawTextG(g, L"Processing setup", 0.f, (float)TOOLBAR_H + 16.f, (float)rc.right, 36.f, 24.f, C_TEXT, true, 0);

        RectI m1 = ModeBtn1(rc), m2 = ModeBtn2(rc), m3 = ModeBtn3(rc);
        DrawButton(g, m1, L"Manga BW", g_mode != DenoiseMode::MangaBW_Sharp, false);
        DrawButton(g, m2, L"Color", g_mode != DenoiseMode::Color_Clean, false);
        DrawButton(g, m3, L"Balanced", g_mode != DenoiseMode::Balanced, false);

        RectI outPick = ProcOutPick(rc);
        SolidBrush box(Color(255, 28, 28, 28));
        Pen br(C_BORDER, 1.0f);
        g.FillRectangle(&box, outPick.x, outPick.y, outPick.w, outPick.h);
        g.DrawRectangle(&br, outPick.x, outPick.y, outPick.w, outPick.h);
        g.DrawLine(&acc, outPick.x, outPick.y + outPick.h - 1, outPick.x + outPick.w, outPick.y + outPick.h - 1);

        std::wstring outLine = g_outputFolder.empty() ? L"Choose output folder..." : EllipsizePath(g_outputFolder, 70);
        DrawTextG(g, outLine, (float)outPick.x + 10.f, (float)outPick.y, (float)outPick.w - 20.f, (float)outPick.h,
            13.f, g_outputFolder.empty() ? C_SUB : C_TEXT, false, -1);

        RectI wm = WidthMinus(rc), wb = WidthBox(rc), wp = WidthPlus(rc);
        DrawButton(g, wm, L"-", false);
        DrawButton(g, wp, L"+", false);

        SolidBrush wbg(Color(255, 28, 28, 28));
        g.FillRectangle(&wbg, wb.x, wb.y, wb.w, wb.h);
        g.DrawRectangle(&br, wb.x, wb.y, wb.w, wb.h);
        g.DrawLine(&acc, wb.x, wb.y + wb.h - 1, wb.x + wb.w, wb.y + wb.h - 1);

        DrawTextG(g, std::to_wstring(g_targetWidth) + L" px", (float)wb.x, (float)wb.y, (float)wb.w, (float)wb.h, 13.f, C_TEXT, false, 0);
        DrawTextG(g, L"Output width", (float)(wb.x + wb.w + 12), (float)wb.y, 240.f, (float)wb.h, 13.f, C_SUB, false, -1);

        int barX = PAD;
        int barY = TOOLBAR_H + 300;
        int barW = rc.right - PAD * 2;
        int barH = 18;

        SolidBrush barBg(Color(255, 40, 40, 40));
        g.FillRectangle(&barBg, barX, barY, barW, barH);

        int total = max(1, (int)g_images.size());
        int done = ClampI(g_processed, 0, total);
        float k = (float)done / (float)total;
        int fillW = (int)std::lround((float)barW * k);

        SolidBrush barFill(C_ACC);
        g.FillRectangle(&barFill, barX, barY, fillW, barH);
        g.DrawRectangle(&br, barX, barY, barW, barH);

        std::wstring ptxt = L"Processed: " + std::to_wstring(done) + L" / " + std::to_wstring((int)g_images.size());
        DrawTextG(g, ptxt, (float)PAD, (float)barY + 26.f, (float)(rc.right - PAD * 2), 26.f, 14.f, C_SUB, false, -1);
    }
    else
    {
        DrawTextG(g, L"All done!", 0.f, (float)TOOLBAR_H + 24.f, (float)rc.right, 44.f, 28.f, C_TEXT, true, 0);

        wchar_t buf[128]{};
        swprintf_s(buf, L"Total time: %.2f s", g_totalSeconds);
        DrawTextG(g, buf, 0.f, (float)TOOLBAR_H + 62.f, (float)rc.right, 28.f, 14.f, C_SUB, false, 0);

        DrawTextG(g, L"Your cat approves. Click “Process more” to start again.", 0.f, (float)TOOLBAR_H + 90.f, (float)rc.right, 28.f, 14.f, C_SUB, false, 0);

        int cx = rc.right / 2;
        int cy = TOOLBAR_H + 360;
        DrawCat(g, cx, cy, g_doneTick);
    }
}

static void HandleFooterRight(HWND hWnd, const RECT& rc)
{
    if (g_screen == Screen::Gallery) {
        if (!g_images.empty()) {
            g_screen = Screen::Processing;
            g_status = L"Choose output folder";
        }
        InvalidateRect(hWnd, nullptr, TRUE);
        return;
    }

    if (g_screen == Screen::Processing) {
        if (!g_outputFolder.empty() && !g_images.empty() && !g_processing) {
            StartProcessing(hWnd);
        }
        else {
            g_status = g_outputFolder.empty() ? L"Select output folder first" : L"No images to process";
        }
        InvalidateRect(hWnd, nullptr, TRUE);
        return;
    }

    ResetToInitial(hWnd);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        DragAcceptFiles(hWnd, TRUE);
        SetTimer(hWnd, TIMER_SCROLL, 16, nullptr);
        return 0;

    case WM_SIZE:
        InvalidateRect(hWnd, nullptr, TRUE);
        return 0;

    case WM_DROPFILES:
    {
        HDROP hDrop = (HDROP)wParam;
        LoadFromDropped(hDrop);
        DragFinish(hDrop);
        InvalidateRect(hWnd, nullptr, TRUE);
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        if (g_screen == Screen::Gallery) {
            short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            g_scrollTarget -= (delta / 120) * 150;
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == TIMER_SCROLL) {
            RECT rc; GetClientRect(hWnd, &rc);
            ComputeScroll(rc);

            int diff = g_scrollTarget - g_scrollY;
            g_scrollY += diff / 4;
            if (abs(diff) < 2) g_scrollY = g_scrollTarget;
            g_scrollY = ClampI(g_scrollY, 0, g_scrollMax);

            RECT inv{ 0, TOOLBAR_H, rc.right, rc.bottom - FOOTER_H };
            InvalidateRect(hWnd, &inv, FALSE);
        }
        else if (wParam == TIMER_PROCESS) {
            if (!g_processing) return 0;

            if (g_processed < (int)g_images.size()) {
                ProcessOne(hWnd);
                RECT rc; GetClientRect(hWnd, &rc);
                RECT inv{ 0, TOOLBAR_H, rc.right, rc.bottom - FOOTER_H };
                InvalidateRect(hWnd, &inv, FALSE);
                InvalidateRect(hWnd, nullptr, FALSE);
            }

            if (g_processed >= (int)g_images.size()) {
                StopProcessing(hWnd);
                SwitchToDone(hWnd);
                InvalidateRect(hWnd, nullptr, TRUE);
            }
        }
        else if (wParam == TIMER_DONE_ANIM) {
            g_doneTick++;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN:
    {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);

        RECT rc; GetClientRect(hWnd, &rc);

        RectI btnAdd = ToolbarBtnAdd(rc);
        RectI btnNext = ToolbarBtnNext(rc);

        RectI btnBack = FooterBtnBack(rc);
        RectI btnRight = FooterBtnRight(rc);

        if (my < TOOLBAR_H) {
            if (PtIn(btnAdd, mx, my)) {
                std::wstring picked = PickFolderDialog(hWnd, L"Select folder with images");
                if (!picked.empty()) {
                    g_inputFolder = picked;
                    std::vector<std::wstring> collected;
                    AddImagesFromFolder(picked, collected);
                    g_images = std::move(collected);
                    std::sort(g_images.begin(), g_images.end());
                    g_thumbs.clear();
                    g_thumbs.resize(g_images.size());

                    ResetScroll();
                    g_outputFolder.clear();
                    g_processing = false;
                    g_processed = 0;
                    g_totalSeconds = 0.0;

                    g_screen = Screen::Gallery;
                    g_status = g_images.empty() ? L"No images found" : L"Images loaded";
                    InvalidateRect(hWnd, nullptr, TRUE);
                }
                return 0;
            }

            if (PtIn(btnNext, mx, my)) {
                if (!g_images.empty()) {
                    g_screen = Screen::Processing;
                    g_status = L"Choose output folder";
                    InvalidateRect(hWnd, nullptr, TRUE);
                }
                return 0;
            }
        }

        if (my >= rc.bottom - FOOTER_H) {
            if (g_screen != Screen::Gallery && PtIn(btnBack, mx, my)) {
                if (g_screen == Screen::Processing && g_processing) StopProcessing(hWnd);
                g_screen = Screen::Gallery;
                g_status = L"Ready";
                InvalidateRect(hWnd, nullptr, TRUE);
                return 0;
            }

            if (PtIn(btnRight, mx, my)) {
                HandleFooterRight(hWnd, rc);
                return 0;
            }
        }

        if (g_screen == Screen::Processing) {
            RectI outPick = ProcOutPick(rc);
            RectI m1 = ModeBtn1(rc), m2 = ModeBtn2(rc), m3 = ModeBtn3(rc);
            RectI wm = WidthMinus(rc), wb = WidthBox(rc), wp = WidthPlus(rc);

            if (PtIn(m1, mx, my)) { SetMode(DenoiseMode::MangaBW_Sharp); InvalidateRect(hWnd, nullptr, TRUE); return 0; }
            if (PtIn(m2, mx, my)) { SetMode(DenoiseMode::Color_Clean);   InvalidateRect(hWnd, nullptr, TRUE); return 0; }
            if (PtIn(m3, mx, my)) { SetMode(DenoiseMode::Balanced);     InvalidateRect(hWnd, nullptr, TRUE); return 0; }

            if (PtIn(outPick, mx, my)) {
                std::wstring picked = PickFolderDialog(hWnd, L"Select output folder");
                if (!picked.empty()) {
                    g_outputFolder = picked;
                    g_status = L"Output folder selected";
                    InvalidateRect(hWnd, nullptr, TRUE);
                }
                return 0;
            }

            if (PtIn(wm, mx, my)) {
                g_targetWidth = ClampI(g_targetWidth - 100, 200, 8000);
                g_status = L"Width updated";
                InvalidateRect(hWnd, nullptr, TRUE);
                return 0;
            }
            if (PtIn(wp, mx, my)) {
                g_targetWidth = ClampI(g_targetWidth + 100, 200, 8000);
                g_status = L"Width updated";
                InvalidateRect(hWnd, nullptr, TRUE);
                return 0;
            }
            if (PtIn(wb, mx, my)) {
                g_status = L"Use +/- to change width";
                InvalidateRect(hWnd, nullptr, TRUE);
                return 0;
            }
        }

        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rc; GetClientRect(hWnd, &rc);
        EnsureBackbuffer(hdc, rc.right, rc.bottom);

        Graphics g(g_backDC);
        DrawScreen(g, rc);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, g_backDC, 0, 0, SRCCOPY);

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_SCROLL);
        KillTimer(hWnd, TIMER_PROCESS);
        KillTimer(hWnd, TIMER_DONE_ANIM);

        if (g_backDC) { DeleteDC(g_backDC); g_backDC = nullptr; }
        if (g_backBmp) { DeleteObject(g_backBmp); g_backBmp = nullptr; }

        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    QueryPerformanceFrequency(&g_qpf);

    GdiplusStartupInput gd;
    GdiplusStartup(&g_gdiplusToken, &gd, nullptr);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = WINDOW_CLASS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(24, 24, 24));
    RegisterClassW(&wc);

    HWND hWnd = CreateWindowW(
        WINDOW_CLASS,
        WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1200, 800,
        nullptr, nullptr,
        hInst, nullptr
    );

    ShowWindow(hWnd, nCmdShow);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(g_gdiplusToken);
    CoUninitialize();
    return 0;
}
