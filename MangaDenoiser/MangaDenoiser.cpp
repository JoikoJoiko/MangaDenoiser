#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <objbase.h>
#include <gdiplus.h>

#include <string>
#include <vector>
#include <algorithm>
using std::min;
using std::max;
#include <memory>
#include <cwctype>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Comdlg32.lib")

using namespace Gdiplus;

// ------------------------------
// Const / UI metrics
// ------------------------------
constexpr wchar_t WINDOW_CLASS[] = L"MangaDenoiserWindow";
constexpr wchar_t WINDOW_TITLE[] = L"Manga Tools";

constexpr int TOOLBAR_H = 56;
constexpr int FOOTER_H = 44;

constexpr int PAD = 16;
constexpr int CELL = 140;
constexpr int GAP = 16;

constexpr UINT_PTR TIMER_UI = 1;
constexpr UINT_PTR TIMER_ANIM = 2;

constexpr UINT WM_APP_PROGRESS = WM_APP + 1;
constexpr UINT WM_APP_DONE = WM_APP + 2;

// ------------------------------
// Small helpers
// ------------------------------
struct RectI { int x, y, w, h; };
static inline bool PtIn(const RectI& r, int px, int py) { return px >= r.x && px < (r.x + r.w) && py >= r.y && py < (r.y + r.h); }
static inline int ClampI(int v, int a, int b) { return (v < a) ? a : (v > b) ? b : v; }
static inline int iabs(int v) { return v < 0 ? -v : v; }

// ------------------------------
// Colors
// ------------------------------
static Color C_BG(255, 24, 24, 24);
static Color C_PANEL(255, 32, 32, 32);
static Color C_BTN(255, 55, 55, 55);
static Color C_BTN_DIS(255, 40, 40, 40);
static Color C_BORDER(255, 92, 92, 92);
static Color C_TEXT(255, 225, 225, 225);
static Color C_SUB(255, 175, 175, 175);
static Color C_ACC(255, 255, 120, 205);

// ------------------------------
// State enums
// ------------------------------
enum class View { Home, Pick, Setup, Processing, Done };
enum class Tool { None, Denoise, Merge, Rename };
enum class DenoiseMode { Manga, Color, Balanced };

enum class EditField { None, MergeName, RenPrefix, RenSuffix };

// ------------------------------
// Globals
// ------------------------------
static ULONG_PTR g_gdiplusToken = 0;

static View g_view = View::Home;
static Tool g_tool = Tool::None;

static std::vector<std::wstring> g_inputs;
static std::vector<std::unique_ptr<Bitmap>> g_thumbs;

static std::wstring g_inputFolder;
static std::wstring g_outputFolder;

static std::wstring g_status = L"Ready";

static int g_scrollY = 0;
static int g_scrollTarget = 0;
static int g_scrollMax = 0;

static int g_animTick = 0;

static bool g_processing = false;
static int g_processed = 0;
static int g_total = 0;
static long long g_elapsedMs = 0;

// Denoise options
static int g_outWidth = 1600;
static DenoiseMode g_dnMode = DenoiseMode::Manga;

// Merge options
static std::wstring g_mergeOutFolder;
static bool g_mergeJpeg = false;
static std::wstring g_mergeName = L"merged";

// Rename options
static std::wstring g_renPrefix = L"image_";
static std::wstring g_renSuffix = L"";
static int g_renStart = 1;
static int g_renPad = 3;

// Tooltip
static bool g_ttShow = false;
static std::wstring g_ttText;
static POINT g_ttPos{ 0,0 };
static int g_ttId = -1;

// Inline edit
static EditField g_editField = EditField::None;
static std::wstring g_editBuf;

// Thread
static HANDLE g_worker = nullptr;
static HWND g_hWndMain = nullptr;

// ------------------------------
// Forward decl
// ------------------------------
static std::wstring GetExtLower(const std::wstring& path);
static std::wstring GetFileNameOnly(const std::wstring& path);
static std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
static std::wstring EllipsizePath(const std::wstring& s, int maxChars);

// ------------------------------
// Status / formatting
// ------------------------------
static void SetStatus(const std::wstring& s) { g_status = s; }

static std::wstring FormatDuration(long long ms)
{
    if (ms < 0) ms = 0;
    long long sec = ms / 1000;
    if (sec < 60) return std::to_wstring((int)sec) + L" s";
    long long m = sec / 60;
    long long s = sec % 60;
    std::wstring out = std::to_wstring((int)m) + L" min ";
    if (s < 10) out += L"0";
    out += std::to_wstring((int)s) + L" s";
    return out;
}

// ------------------------------
// Files
// ------------------------------
static bool IsImageFile(const std::wstring& p)
{
    std::wstring ext = GetExtLower(p);
    return ext == L"png" || ext == L"jpg" || ext == L"jpeg" || ext == L"bmp" || ext == L"webp";
}

static void CollectFromFolder(const std::wstring& folder, std::vector<std::wstring>& out, bool allFiles)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((folder + L"\\*.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring f = folder + L"\\" + fd.cFileName;
        if (allFiles) out.push_back(f);
        else if (IsImageFile(f)) out.push_back(f);
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
    if (!SHGetPathFromIDListW(pidl, path))
    {
        CoTaskMemFree(pidl);
        return L"";
    }
    CoTaskMemFree(pidl);
    return path;
}

// ------------------------------
// Layout helpers
// ------------------------------
static int ComputeCols(int clientW)
{
    int usable = clientW - PAD * 2;
    int step = CELL + GAP;
    return max(1, usable / step);
}

static int ComputeContentHeight(int cols, int count)
{
    if (count <= 0) return PAD * 2;
    int rows = (int)((count + cols - 1) / cols);
    int h = PAD * 2 + rows * (CELL + GAP) - GAP;
    return max(h, PAD * 2);
}

static void ResetScroll()
{
    g_scrollY = 0;
    g_scrollTarget = 0;
    g_scrollMax = 0;
}

static void ComputeScrollMax(const RECT& rc)
{
    int viewTop = TOOLBAR_H;
    int viewBottom = rc.bottom - FOOTER_H;
    int viewH = max(0, viewBottom - viewTop);

    int cols = ComputeCols(rc.right);
    int contentH = ComputeContentHeight(cols, (int)g_inputs.size());
    g_scrollMax = max(0, contentH - viewH);

    g_scrollTarget = ClampI(g_scrollTarget, 0, g_scrollMax);
    g_scrollY = ClampI(g_scrollY, 0, g_scrollMax);
}

// ------------------------------
// Strings helpers
// ------------------------------
static std::wstring GetFileNameOnly(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return path;
    return path.substr(p + 1);
}

static std::wstring GetExtLower(const std::wstring& path)
{
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return L"";
    std::wstring e = path.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(), ::towlower);
    return e;
}

static std::wstring JoinPath(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

static std::wstring EllipsizePath(const std::wstring& s, int maxChars)
{
    if ((int)s.size() <= maxChars) return s;
    if (maxChars < 10) return s.substr(0, maxChars);
    int keepL = maxChars / 2 - 2;
    int keepR = maxChars - keepL - 3;
    return s.substr(0, keepL) + L"..." + s.substr((int)s.size() - keepR);
}

static std::wstring PadNumber(int v, int width)
{
    std::wstring s = std::to_wstring(v);
    while ((int)s.size() < width) s = L"0" + s;
    return s;
}

// ------------------------------
// GDI+ text / shapes
// ------------------------------
static void DrawTextG(Graphics& g, const std::wstring& text, float x, float y, float w, float h, float size, Color color, bool bold, int alignH)
{
    FontFamily ff(L"Segoe UI");
    Font f(&ff, size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    SolidBrush b(color);
    RectF r(x, y, w, h);
    StringFormat sf;
    sf.SetLineAlignment(StringAlignmentCenter);
    if (alignH < 0) sf.SetAlignment(StringAlignmentNear);
    else if (alignH > 0) sf.SetAlignment(StringAlignmentFar);
    else sf.SetAlignment(StringAlignmentCenter);
    g.DrawString(text.c_str(), -1, &f, r, &sf, &b);
}

static void RoundedPath(GraphicsPath& path, float x, float y, float w, float h, float r)
{
    float d = r * 2.f;
    path.Reset();
    path.AddArc(x, y, d, d, 180.f, 90.f);
    path.AddArc(x + w - d, y, d, d, 270.f, 90.f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.f, 90.f);
    path.AddArc(x, y + h - d, d, d, 90.f, 90.f);
    path.CloseFigure();
}

static void FillRoundRect(Graphics& g, const RectF& r, float radius, Brush& fill, Pen* border)
{
    GraphicsPath path;
    RoundedPath(path, r.X, r.Y, r.Width, r.Height, radius);
    g.FillPath(&fill, &path);
    if (border) g.DrawPath(border, &path);
}

static void DrawButton(Graphics& g, const RectI& r, const wchar_t* text, bool disabled, bool accentLine = true)
{
    SolidBrush bg(disabled ? C_BTN_DIS : C_BTN);
    Pen br(C_BORDER, 1.0f);
    g.FillRectangle(&bg, r.x, r.y, r.w, r.h);
    g.DrawRectangle(&br, r.x, r.y, r.w, r.h);

    if (!disabled && accentLine)
    {
        Pen acc(C_ACC, 2.0f);
        g.DrawLine(&acc, r.x, r.y + r.h - 1, r.x + r.w, r.y + r.h - 1);
    }

    DrawTextG(g, text, (float)r.x, (float)r.y, (float)r.w, (float)r.h, 14.0f,
        disabled ? Color(255, 135, 135, 135) : C_TEXT, false, 0);
}

static void DrawBackArrow(Graphics& g, const RectI& r)
{
    SolidBrush bg(Color(255, 48, 48, 48));
    Pen br(C_BORDER, 1.0f);
    g.FillRectangle(&bg, r.x, r.y, r.w, r.h);
    g.DrawRectangle(&br, r.x, r.y, r.w, r.h);

    Pen acc(C_ACC, 2.0f);
    int cx = r.x + r.w / 2;
    int cy = r.y + r.h / 2;
    g.DrawLine(&acc, cx + 6, cy - 9, cx - 6, cy);
    g.DrawLine(&acc, cx - 6, cy, cx + 6, cy + 9);
    g.DrawLine(&acc, cx - 6, cy, cx + 10, cy);
}

static void DrawInfoIcon(Graphics& g, const RectI& r)
{
    SolidBrush bg(Color(255, 40, 40, 40));
    Pen br(C_BORDER, 1.0f);
    Pen acc(C_ACC, 2.0f);
    SolidBrush dot(C_ACC);

    g.FillEllipse(&bg, r.x, r.y, r.w, r.h);
    g.DrawEllipse(&br, r.x, r.y, r.w, r.h);

    int cx = r.x + r.w / 2;
    int cy = r.y + r.h / 2;

    g.DrawLine(&acc, cx, cy - 6, cx, cy + 3);
    g.FillEllipse(&dot, cx - 2, cy + 6, 4, 4);
}

static void DrawWindowAccent(Graphics& g, int w, int h)
{
    Pen acc(C_ACC, 2.0f);
    g.DrawRectangle(&acc, 1, 1, w - 3, h - 3);
}

// ------------------------------
// Tooltip
// ------------------------------
static void DrawTooltip(Graphics& g, int clientW, int clientH)
{
    if (!g_ttShow || g_ttText.empty()) return;

    int x = g_ttPos.x + 14;
    int y = g_ttPos.y + 18;

    int maxW = min(520, clientW - 20);
    int pad = 10;

    FontFamily ff(L"Segoe UI");
    Font f(&ff, 13.f, FontStyleRegular, UnitPixel);

    RectF layout(0, 0, (REAL)maxW, 2000);
    RectF bounds;
    StringFormat sf;
    sf.SetAlignment(StringAlignmentNear);
    sf.SetLineAlignment(StringAlignmentNear);

    g.MeasureString(g_ttText.c_str(), -1, &f, layout, &sf, &bounds);

    int w = (int)bounds.Width + pad * 2;
    int h = (int)bounds.Height + pad * 2;

    if (x + w > clientW - 10) x = clientW - 10 - w;
    if (y + h > clientH - 10) y = clientH - 10 - h;
    if (x < 10) x = 10;
    if (y < 10) y = 10;

    RectF rr((REAL)x, (REAL)y, (REAL)w, (REAL)h);
    SolidBrush fill(Color(245, 34, 34, 34));
    Pen br(C_ACC, 2.f);
    FillRoundRect(g, rr, 10.f, fill, &br);

    SolidBrush txt(Color(255, 235, 235, 235));
    RectF tr(rr.X + (REAL)pad, rr.Y + (REAL)pad, rr.Width - (REAL)pad * 2, rr.Height - (REAL)pad * 2);
    g.DrawString(g_ttText.c_str(), -1, &f, tr, &sf, &txt);
}

static void UpdateTooltipByMouse(int mx, int my)
{
    bool was = g_ttShow;

    g_ttShow = false;
    g_ttText.clear();
    g_ttId = -1;

    if (g_view != View::Setup || g_tool != Tool::Denoise)
    {
        if (was) InvalidateRect(g_hWndMain, nullptr, FALSE);
        return;
    }

    int y = TOOLBAR_H + 28;
    int y2 = y + 92;
    int y3 = y2 + 52;

    RectI i1{ PAD + 292, y3 + 32, 18, 18 };
    RectI i2{ PAD + 292, y3 + 76, 18, 18 };
    RectI i3{ PAD + 292, y3 + 120, 18, 18 };

    if (PtIn(i1, mx, my))
    {
        g_ttShow = true;
        g_ttId = 1;
        g_ttText = L"Manga: keeps crisp linework. Median luma + stronger unsharp.";
    }
    else if (PtIn(i2, mx, my))
    {
        g_ttShow = true;
        g_ttId = 2;
        g_ttText = L"Color: smoother gradients, less color noise. Mild blur + medium unsharp.";
    }
    else if (PtIn(i3, mx, my))
    {
        g_ttShow = true;
        g_ttId = 3;
        g_ttText = L"Balanced: safe default for mixed pages.";
    }

    if (g_ttShow)
    {
        g_ttPos.x = mx;
        g_ttPos.y = my;
    }

    if (was != g_ttShow) InvalidateRect(g_hWndMain, nullptr, FALSE);
}

// ------------------------------
// Encoders / bitmap IO
// ------------------------------
static CLSID GetEncoderClsid(const WCHAR* format)
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

static CLSID EncoderForPath(const std::wstring& outPath)
{
    std::wstring ext = GetExtLower(outPath);
    if (ext == L"jpg" || ext == L"jpeg") return GetEncoderClsid(L"image/jpeg");
    return GetEncoderClsid(L"image/png");
}

static bool SaveBitmap(Bitmap* bmp, const std::wstring& outPath)
{
    if (!bmp || bmp->GetLastStatus() != Ok) return false;
    CLSID enc = EncoderForPath(outPath);
    if (enc == CLSID{}) return false;
    Status s = bmp->Save(outPath.c_str(), &enc, nullptr);
    return s == Ok;
}

// ensures output extension is png/jpg when source is unsupported (webp/bmp etc)
static std::wstring NormalizeOutputNameForSave(const std::wstring& fileName, bool preferJpegIfJpg)
{
    std::wstring ext = GetExtLower(fileName);
    if (ext == L"png" || ext == L"jpg" || ext == L"jpeg")
        return fileName;

    size_t dot = fileName.find_last_of(L'.');
    if (dot == std::wstring::npos)
        return fileName + (preferJpegIfJpg ? L".jpg" : L".png");

    return fileName.substr(0, dot) + (preferJpegIfJpg ? L".jpg" : L".png");
}

// ------------------------------
// Resize (width -> height auto)
// ------------------------------
static std::unique_ptr<Bitmap> ResizeToWidth(Bitmap* src, int targetW)
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

// ------------------------------
// Denoise routines (simple but stable)
// ------------------------------
static void Lock32(Bitmap* bmp, BitmapData& bd)
{
    Rect r(0, 0, (INT)bmp->GetWidth(), (INT)bmp->GetHeight());
    bmp->LockBits(&r, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppARGB, &bd);
}

static inline int iclamp(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }

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
    {
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
                    sumB += p[0];
                    sumG += p[1];
                    sumR += p[2];
                    cnt++;
                }
            }

            BYTE* o = dp(x, y);
            int blurB = sumB / cnt;
            int blurG = sumG / cnt;
            int blurR = sumR / cnt;

            int b = (int)o[0];
            int g = (int)o[1];
            int rr = (int)o[2];

            int nb = iclamp((int)(b + (b - blurB) * amount), 0, 255);
            int ng = iclamp((int)(g + (g - blurG) * amount), 0, 255);
            int nr = iclamp((int)(rr + (rr - blurR) * amount), 0, 255);

            o[0] = (BYTE)nb;
            o[1] = (BYTE)ng;
            o[2] = (BYTE)nr;
        }
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

    auto sp = [&](int x, int y)->BYTE* { return src.data() + (size_t)y * (size_t)stride + (size_t)x * 4; };
    auto dp = [&](int x, int y)->BYTE* { return dst0 + (size_t)y * (size_t)stride + (size_t)x * 4; };

    int passes = iclamp(strength, 1, 3);

    for (int pass = 0; pass < passes; pass++)
    {
        std::vector<BYTE> src2(bufSize);
        memcpy(src2.data(), dst0, bufSize);
        auto sp2 = [&](int x, int y)->BYTE* { return src2.data() + (size_t)y * (size_t)stride + (size_t)x * 4; };

        for (int y = 0; y < h; y++)
        {
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
    {
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
                    sumB += p[0];
                    sumG += p[1];
                    sumR += p[2];
                    cnt++;
                }
            }
            BYTE* o = dp(x, y);
            o[0] = (BYTE)(sumB / cnt);
            o[1] = (BYTE)(sumG / cnt);
            o[2] = (BYTE)(sumR / cnt);
        }
    }

    bmp->UnlockBits(&bd);
}

static void ApplyDenoise(Bitmap* bmp, DenoiseMode mode)
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

// ------------------------------
// Thumbs
// ------------------------------
static std::unique_ptr<Bitmap> BuildThumb(const std::wstring& path, bool allowImage)
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

    DrawTextG(gg, ext, 0.f, 0.f, (float)CELL, (float)CELL, 18.f, C_SUB, true, 0);
    return thumb;
}

static void EnsureThumb(size_t i)
{
    if (i >= g_thumbs.size()) return;
    if (g_thumbs[i]) return;
    bool allowImage = (g_tool != Tool::Rename);
    g_thumbs[i] = BuildThumb(g_inputs[i], allowImage);
}

// ------------------------------
// Reset states
// ------------------------------
static void ClearAllStateToHome()
{
    g_view = View::Home;
    g_tool = Tool::None;

    g_inputs.clear();
    g_thumbs.clear();

    g_inputFolder.clear();
    g_outputFolder.clear();

    g_status = L"Ready";
    g_processing = false;
    g_processed = 0;
    g_total = 0;
    g_elapsedMs = 0;

    g_editField = EditField::None;
    g_editBuf.clear();

    ResetScroll();
}

static void ClearToolStateKeepTool()
{
    g_inputs.clear();
    g_thumbs.clear();
    g_inputFolder.clear();
    g_outputFolder.clear();

    g_status = L"Ready";
    g_processing = false;
    g_processed = 0;
    g_total = 0;
    g_elapsedMs = 0;

    g_editField = EditField::None;
    g_editBuf.clear();

    ResetScroll();
}

// ------------------------------
// Load inputs
// ------------------------------
static void LoadInputsFromFolder(const std::wstring& folder)
{
    g_inputs.clear();
    g_thumbs.clear();

    bool allFiles = (g_tool == Tool::Rename);
    CollectFromFolder(folder, g_inputs, allFiles);

    std::sort(g_inputs.begin(), g_inputs.end());

    g_thumbs.resize(g_inputs.size());
    for (auto& t : g_thumbs) t.reset();

    g_inputFolder = folder;
    ResetScroll();

    SetStatus(g_inputs.empty() ? L"No files found" : L"Loaded");
}

static void LoadInputsFromDrop(HDROP hDrop)
{
    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    wchar_t path[MAX_PATH]{};

    std::vector<std::wstring> collected;
    bool allFiles = (g_tool == Tool::Rename);

    for (UINT i = 0; i < count; i++)
    {
        if (!DragQueryFileW(hDrop, i, path, MAX_PATH)) continue;
        std::wstring p = path;
        DWORD attr = GetFileAttributesW(p.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;

        if (attr & FILE_ATTRIBUTE_DIRECTORY)
        {
            g_inputFolder = p;
            CollectFromFolder(p, collected, allFiles);
        }
        else
        {
            if (allFiles) collected.push_back(p);
            else if (IsImageFile(p)) collected.push_back(p);
        }
    }

    g_inputs = std::move(collected);
    std::sort(g_inputs.begin(), g_inputs.end());

    g_thumbs.clear();
    g_thumbs.resize(g_inputs.size());
    for (auto& t : g_thumbs) t.reset();

    ResetScroll();
    SetStatus(g_inputs.empty() ? L"No files found" : L"Loaded");
}

// ------------------------------
// Worker thread
// ------------------------------
static LARGE_INTEGER QPFreq()
{
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f;
}

static long long NowMs()
{
    static LARGE_INTEGER f = QPFreq();
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return (long long)(t.QuadPart * 1000LL / f.QuadPart);
}

struct WorkerCtx
{
    Tool tool = Tool::None;
    DenoiseMode mode = DenoiseMode::Manga;

    std::vector<std::wstring> inputs;

    std::wstring outFolder;   // denoise/rename
    std::wstring mergePath;   // merge
    int outWidth = 1600;      // denoise

    std::wstring renPrefix;
    std::wstring renSuffix;
    int renStart = 1;
    int renPad = 3;

    HWND hWnd = nullptr;
};

static DWORD WINAPI WorkerProc(LPVOID p)
{
    std::unique_ptr<WorkerCtx> ctx((WorkerCtx*)p);

    long long t0 = NowMs();
    int total = (int)ctx->inputs.size();

    PostMessageW(ctx->hWnd, WM_APP_PROGRESS, 0, (LPARAM)total);

    if (ctx->tool == Tool::Denoise)
    {
        for (int i = 0; i < total; i++)
        {
            std::unique_ptr<Bitmap> src(new Bitmap(ctx->inputs[i].c_str()));
            if (src && src->GetLastStatus() == Ok)
            {
                auto resized = ResizeToWidth(src.get(), ctx->outWidth);
                if (resized)
                {
                    ApplyDenoise(resized.get(), ctx->mode);

                    std::wstring base = GetFileNameOnly(ctx->inputs[i]);
                    // если исходник webp/bmp — делаем норм расширение, иначе Save может не сработать
                    base = NormalizeOutputNameForSave(base, false);

                    std::wstring outPath = JoinPath(ctx->outFolder, base);
                    SaveBitmap(resized.get(), outPath);
                }
            }
            PostMessageW(ctx->hWnd, WM_APP_PROGRESS, (WPARAM)(i + 1), (LPARAM)total);
        }
    }
    else if (ctx->tool == Tool::Merge)
    {
        // load images + find max width
        int maxW = 0;
        std::vector<std::unique_ptr<Bitmap>> imgs;
        imgs.reserve(total);

        for (int i = 0; i < total; i++)
        {
            std::unique_ptr<Bitmap> b(new Bitmap(ctx->inputs[i].c_str()));
            if (b && b->GetLastStatus() == Ok)
            {
                int w = (int)b->GetWidth();
                if (w > maxW) maxW = w;
                imgs.push_back(std::move(b));
            }
            else imgs.push_back(nullptr);

            PostMessageW(ctx->hWnd, WM_APP_PROGRESS, (WPARAM)(i + 1), (LPARAM)total);
        }

        if (maxW > 0)
        {
            long long totalH = 0;
            std::vector<std::pair<int, int>> sizes;
            sizes.reserve(imgs.size());

            for (auto& b : imgs)
            {
                if (!b || b->GetLastStatus() != Ok) { sizes.push_back({ 0,0 }); continue; }
                int sw = (int)b->GetWidth();
                int sh = (int)b->GetHeight();
                float k = (float)maxW / (float)sw;
                int nh = max(1, (int)(sh * k + 0.5f));
                sizes.push_back({ maxW, nh });
                totalH += nh;
            }

            if (totalH > 0 && totalH < 400000) // safety
            {
                std::unique_ptr<Bitmap> out(new Bitmap(maxW, (int)totalH, PixelFormat32bppARGB));
                if (out && out->GetLastStatus() == Ok)
                {
                    Graphics g(out.get());
                    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                    g.SetSmoothingMode(SmoothingModeHighQuality);
                    g.Clear(Color(255, 255, 255, 255));

                    int y = 0;
                    for (size_t i = 0; i < imgs.size(); i++)
                    {
                        auto& b = imgs[i];
                        if (!b || b->GetLastStatus() != Ok) continue;

                        int sw = (int)b->GetWidth();
                        int sh = (int)b->GetHeight();
                        int dw = sizes[i].first;
                        int dh = sizes[i].second;

                        g.DrawImage(b.get(), Rect(0, y, dw, dh), 0, 0, sw, sh, UnitPixel);
                        y += dh;
                    }

                    SaveBitmap(out.get(), ctx->mergePath);
                }
            }
        }
    }
    else if (ctx->tool == Tool::Rename)
    {
        for (int i = 0; i < total; i++)
        {
            std::wstring ext = L"";
            size_t dot = ctx->inputs[i].find_last_of(L'.');
            if (dot != std::wstring::npos) ext = ctx->inputs[i].substr(dot);

            int n = ctx->renStart + i;
            std::wstring num = PadNumber(n, ctx->renPad);

            std::wstring outName = ctx->renPrefix + num + ctx->renSuffix + ext;
            std::wstring outPath = JoinPath(ctx->outFolder, outName);

            CopyFileW(ctx->inputs[i].c_str(), outPath.c_str(), FALSE);

            PostMessageW(ctx->hWnd, WM_APP_PROGRESS, (WPARAM)(i + 1), (LPARAM)total);
        }
    }

    long long t1 = NowMs();
    PostMessageW(ctx->hWnd, WM_APP_DONE, 0, (LPARAM)(t1 - t0));
    return 0;
}

static void StopWorkerIfAny()
{
    if (g_worker)
    {
        CloseHandle(g_worker);
        g_worker = nullptr;
    }
}

static void StartWorker(HWND hWnd)
{
    if (g_processing) return;
    if (g_inputs.empty()) return;

    // finalize edit if any
    g_editField = EditField::None;
    g_editBuf.clear();

    // validate
    if (g_tool == Tool::Denoise || g_tool == Tool::Rename)
    {
        if (g_outputFolder.empty()) { SetStatus(L"Select output folder first"); return; }
    }
    if (g_tool == Tool::Merge)
    {
        if (g_mergeOutFolder.empty()) { SetStatus(L"Select output folder first"); return; }
        if (g_mergeName.empty()) g_mergeName = L"merged";

        std::wstring ext = g_mergeJpeg ? L".jpg" : L".png";
        // if user typed extension manually, normalize to chosen
        std::wstring base = g_mergeName;
        // strip .ext if any
        size_t dot = base.find_last_of(L'.');
        if (dot != std::wstring::npos) base = base.substr(0, dot);
        std::wstring outName = base + ext;

        // merge path
        // NOTE: we store it locally in ctx
        // no global needed
        // (but useful for status)
    }

    g_processing = true;
    g_processed = 0;
    g_total = (int)g_inputs.size();
    g_elapsedMs = 0;

    SetStatus(L"Processing...");
    g_view = View::Processing;

    auto ctx = new WorkerCtx();
    ctx->tool = g_tool;
    ctx->mode = g_dnMode;
    ctx->inputs = g_inputs;
    ctx->outFolder = g_outputFolder;
    ctx->outWidth = g_outWidth;
    ctx->hWnd = hWnd;

    ctx->renPrefix = g_renPrefix;
    ctx->renSuffix = g_renSuffix;
    ctx->renStart = g_renStart;
    ctx->renPad = g_renPad;

    if (g_tool == Tool::Merge)
    {
        std::wstring ext = g_mergeJpeg ? L".jpg" : L".png";
        std::wstring base = g_mergeName;
        size_t dot = base.find_last_of(L'.');
        if (dot != std::wstring::npos) base = base.substr(0, dot);
        ctx->mergePath = JoinPath(g_mergeOutFolder, base + ext);
    }

    DWORD tid = 0;
    g_worker = CreateThread(nullptr, 0, WorkerProc, ctx, 0, &tid);
}

// ------------------------------
// Toolbar / Footer
// ------------------------------
static void DrawToolbarCommon(Graphics& g, int w, bool showBackToHome)
{
    SolidBrush tb(C_PANEL);
    g.FillRectangle(&tb, 0, 0, w, TOOLBAR_H);

    Pen acc(C_ACC, 2.f);
    g.DrawLine(&acc, 0, TOOLBAR_H - 1, w, TOOLBAR_H - 1);

    if (showBackToHome)
    {
        RectI back{ PAD, 12, 42, 32 };
        DrawBackArrow(g, back);
    }
}

static void DrawFooter(Graphics& g, int w, int h, bool showBack, bool showNext, const std::wstring& nextText, bool nextDisabled)
{
    SolidBrush fb(C_PANEL);
    g.FillRectangle(&fb, 0, h - FOOTER_H, w, FOOTER_H);

    Pen acc(C_ACC, 2.f);
    g.DrawLine(&acc, 0, h - FOOTER_H, w, h - FOOTER_H);

    RectI btnBack{ PAD, h - FOOTER_H + 6, 120, 32 };
    RectI btnNext{ w - PAD - 160, h - FOOTER_H + 6, 160, 32 };

    int leftX = PAD;

    if (showBack)
    {
        DrawButton(g, btnBack, L"Back", false);
        leftX = btnBack.x + btnBack.w + 16;
    }

    if (showNext)
    {
        DrawButton(g, btnNext, nextText.c_str(), nextDisabled);
    }

    std::wstring line = L"Items: " + std::to_wstring((int)g_inputs.size()) + L"    Status: " + g_status;
    DrawTextG(g, line, (float)leftX, (float)(h - FOOTER_H), (float)(w - leftX - PAD - (showNext ? (btnNext.w + 16) : 0)), (float)FOOTER_H, 13.f, C_SUB, false, -1);
}

// ------------------------------
// Cats
// ------------------------------
static void DrawPeekingCat(Graphics& g, int w, int h, int t)
{
    int baseY = h - 12;
    int cx = w / 2;

    int eyeShift = (int)(std::sin((double)t * 0.08) * 6.0);
    int earWig = (int)(std::sin((double)t * 0.06) * 3.0);

    SolidBrush face(Color(255, 52, 52, 52));
    Pen out(Color(255, 92, 92, 92), 2.f);
    Pen whisk(C_SUB, 2.f);

    RectF head((REAL)(cx - 170), (REAL)(baseY - 120), 340.f, 160.f);
    GraphicsPath path;
    RoundedPath(path, head.X, head.Y, head.Width, head.Height, 56.f); // более "кругло"
    g.FillPath(&face, &path);
    g.DrawPath(&out, &path);

    Point earL[3] = { Point(cx - 110, baseY - 118), Point(cx - 155, baseY - 162 - earWig), Point(cx - 70, baseY - 145) };
    Point earR[3] = { Point(cx + 110, baseY - 118), Point(cx + 155, baseY - 162 + earWig), Point(cx + 70, baseY - 145) };
    g.FillPolygon(&face, earL, 3);
    g.FillPolygon(&face, earR, 3);
    g.DrawPolygon(&out, earL, 3);
    g.DrawPolygon(&out, earR, 3);

    SolidBrush eye(Color(255, 235, 235, 235));
    int ey = baseY - 78;
    g.FillEllipse(&eye, cx - 60 + eyeShift, ey, 18, 18);
    g.FillEllipse(&eye, cx + 42 + eyeShift, ey, 18, 18);

    g.DrawLine(&whisk, cx - 55, baseY - 55, cx - 145, baseY - 70);
    g.DrawLine(&whisk, cx - 55, baseY - 45, cx - 145, baseY - 45);
    g.DrawLine(&whisk, cx - 55, baseY - 35, cx - 145, baseY - 20);

    g.DrawLine(&whisk, cx + 55, baseY - 55, cx + 145, baseY - 70);
    g.DrawLine(&whisk, cx + 55, baseY - 45, cx + 145, baseY - 45);
    g.DrawLine(&whisk, cx + 55, baseY - 35, cx + 145, baseY - 20);
}

static void DrawDoneCat(Graphics& g, int cx, int cy, int t)
{
    SolidBrush face(Color(255, 56, 56, 56));
    SolidBrush body(Color(255, 50, 50, 50));
    Pen out(Color(255, 92, 92, 92), 2.f);
    Pen whisk(C_SUB, 2.f);
    SolidBrush eye(Color(255, 235, 235, 235));
    SolidBrush blush(C_ACC);

    int sway = (int)(std::sin((double)t * 0.07) * 5.0); // мягче

    RectF bodyR((REAL)(cx - 150), (REAL)(cy + 70), 300.f, 170.f);
    GraphicsPath pb;
    RoundedPath(pb, bodyR.X, bodyR.Y, bodyR.Width, bodyR.Height, 78.f);
    g.FillPath(&body, &pb);
    g.DrawPath(&out, &pb);

    RectF headR((REAL)(cx - 140), (REAL)(cy - 120), 280.f, 220.f);
    GraphicsPath ph;
    RoundedPath(ph, headR.X, headR.Y, headR.Width, headR.Height, 74.f);
    g.FillPath(&face, &ph);
    g.DrawPath(&out, &ph);

    Point earL[3] = { Point(cx - 88, cy - 118), Point(cx - 145, cy - 175), Point(cx - 35, cy - 150) };
    Point earR[3] = { Point(cx + 88, cy - 118), Point(cx + 145, cy - 175), Point(cx + 35, cy - 150) };
    g.FillPolygon(&face, earL, 3);
    g.FillPolygon(&face, earR, 3);
    g.DrawPolygon(&out, earL, 3);
    g.DrawPolygon(&out, earR, 3);

    int eyeShift = (int)(std::sin((double)t * 0.10) * 4.0);
    g.FillEllipse(&eye, cx - 58 + eyeShift, cy - 35, 22, 22);
    g.FillEllipse(&eye, cx + 36 + eyeShift, cy - 35, 22, 22);

    g.FillEllipse(&blush, cx - 95, cy - 5, 26, 14);
    g.FillEllipse(&blush, cx + 69, cy - 5, 26, 14);

    g.DrawLine(&out, cx, cy - 6, cx, cy + 12);

    g.DrawLine(&whisk, cx - 45, cy - 10, cx - 130, cy - 25);
    g.DrawLine(&whisk, cx - 45, cy, cx - 130, cy);
    g.DrawLine(&whisk, cx - 45, cy + 10, cx - 130, cy + 25);

    g.DrawLine(&whisk, cx + 45, cy - 10, cx + 130, cy - 25);
    g.DrawLine(&whisk, cx + 45, cy, cx + 130, cy);
    g.DrawLine(&whisk, cx + 45, cy + 10, cx + 130, cy + 25);

    // tail: вместо “жёсткой” линии — широкая и плавная кривая
    Pen tail(C_ACC, 6.f);
    tail.SetStartCap(LineCapRound);
    tail.SetEndCap(LineCapRound);

    int tailX = cx + 120;
    int tailY = cy + 160;

    GraphicsPath tp;
    tp.AddBezier(
        Point(tailX, tailY),
        Point(tailX + 30 + sway, tailY - 10),
        Point(tailX + 55 + sway, tailY + 35),
        Point(tailX + 78, tailY + 18)
    );
    g.DrawPath(&tail, &tp);
}

// ------------------------------
// Home / Pick / Setup / Processing / Done
// ------------------------------
static void DrawHome(Graphics& g, const RECT& rc)
{
    int w = rc.right;
    int h = rc.bottom;

    SolidBrush bg(C_BG);
    g.FillRectangle(&bg, 0, 0, w, h);

    DrawWindowAccent(g, w, h);

    DrawTextG(g, L"Welcome", 0.f, 96.f, (float)w, 44.f, 34.f, C_TEXT, true, 0);
    DrawTextG(g, L"Choose a tool", 0.f, 142.f, (float)w, 28.f, 15.f, C_SUB, false, 0);

    int bw = 180;
    int bx = (w - (bw * 3 + 24 * 2)) / 2;
    int by = 220;

    RectI b1{ bx, by, bw, 44 };
    RectI b2{ bx + bw + 24, by, bw, 44 };
    RectI b3{ bx + (bw + 24) * 2, by, bw, 44 };

    DrawButton(g, b1, L"Denoise", false);
    DrawButton(g, b2, L"Merge", false);
    DrawButton(g, b3, L"Rename", false);

    DrawTextG(g, L"Tip: after choosing a tool, you can drag & drop a folder/files.", 0.f, (float)(by + 64), (float)w, 24.f, 13.f, C_SUB, false, 0);

    DrawPeekingCat(g, w, h, g_animTick);
}

static void DrawPick(Graphics& g, const RECT& rc)
{
    int w = rc.right;
    int h = rc.bottom;

    SolidBrush bg(C_BG);
    g.FillRectangle(&bg, 0, 0, w, h);

    DrawToolbarCommon(g, w, true);

    RectI btnChoose{ PAD + 58, 12, 160, 32 };
    DrawButton(g, btnChoose, L"Choose folder", false);

    std::wstring toolName = (g_tool == Tool::Denoise) ? L"Denoise" : (g_tool == Tool::Merge) ? L"Merge" : L"Rename";
    DrawTextG(g, toolName, (float)(PAD + 230), 0.f, (float)(w - PAD - 230), (float)TOOLBAR_H, 16.f, C_TEXT, true, -1);

    int viewTop = TOOLBAR_H;
    int viewH = h - TOOLBAR_H - FOOTER_H;

    Rect clip(0, viewTop, w, viewH);
    g.SetClip(clip);

    if (g_inputs.empty())
    {
        std::wstring hint = L"Drop a folder or files here\nor use 'Choose folder'";
        DrawTextG(g, hint, 0.f, (float)viewTop, (float)w, (float)viewH, 22.f, C_TEXT, true, 0);
    }
    else
    {
        int cols = ComputeCols(w);
        int step = CELL + GAP;
        int y0 = viewTop + PAD - g_scrollY;

        int firstRow = max(0, (g_scrollY - PAD) / step);
        int rowsVisible = (viewH / step) + 3;

        int totalRows = (int)((g_inputs.size() + cols - 1) / cols);
        int lastRow = min(totalRows - 1, firstRow + rowsVisible);

        for (int row = firstRow; row <= lastRow; row++)
        {
            for (int col = 0; col < cols; col++)
            {
                size_t i = (size_t)row * (size_t)cols + (size_t)col;
                if (i >= g_inputs.size()) break;

                int x = PAD + col * step;
                int y = y0 + row * step;

                EnsureThumb(i);
                Rect r(x, y, CELL, CELL);

                if (g_thumbs[i] && g_thumbs[i]->GetLastStatus() == Ok)
                    g.DrawImage(g_thumbs[i].get(), r);
                else
                {
                    SolidBrush ph(Color(255, 18, 18, 18));
                    Pen br(Color(255, 45, 45, 45), 1.0f);
                    g.FillRectangle(&ph, r);
                    g.DrawRectangle(&br, r);
                }
            }
        }
    }

    g.ResetClip();

    bool canNext = !g_inputs.empty();
    DrawFooter(g, w, h, true, true, L"Next", !canNext);

    DrawWindowAccent(g, w, h);
    DrawTooltip(g, w, h);
}

static std::wstring FieldTextWithCaret(EditField f, const std::wstring& v)
{
    if (g_editField != f) return v;
    // примитивный caret
    return g_editBuf + L"|";
}

static void DrawSetup(Graphics& g, const RECT& rc)
{
    int w = rc.right;
    int h = rc.bottom;

    SolidBrush bg(C_BG);
    g.FillRectangle(&bg, 0, 0, w, h);

    DrawToolbarCommon(g, w, true);

    RectI backHome{ PAD, 12, 42, 32 };
    DrawBackArrow(g, backHome);

    std::wstring toolName =
        (g_tool == Tool::Denoise) ? L"Denoise setup" :
        (g_tool == Tool::Merge) ? L"Merge setup" :
        L"Rename setup";

    DrawTextG(g, toolName, (float)(PAD + 58), 0.f, (float)(w - PAD - 58), (float)TOOLBAR_H, 16.f, C_TEXT, true, -1);

    int y = TOOLBAR_H + 28;

    Pen br(C_BORDER, 1.0f);
    SolidBrush box(Color(255, 28, 28, 28));

    if (g_tool == Tool::Denoise)
    {
        DrawTextG(g, L"Output folder", (float)PAD, (float)y, (float)w, 24.f, 14.f, C_SUB, false, -1);

        RectI pickOut{ PAD, y + 28, 420, 38 };
        g.FillRectangle(&box, pickOut.x, pickOut.y, pickOut.w, pickOut.h);
        g.DrawRectangle(&br, pickOut.x, pickOut.y, pickOut.w, pickOut.h);
        Pen acc(C_ACC, 2.0f);
        g.DrawLine(&acc, pickOut.x, pickOut.y + pickOut.h - 1, pickOut.x + pickOut.w, pickOut.y + pickOut.h - 1);

        std::wstring outLine = g_outputFolder.empty() ? L"Choose output folder..." : EllipsizePath(g_outputFolder, 70);
        DrawTextG(g, outLine, (float)pickOut.x + 10.f, (float)pickOut.y, (float)pickOut.w - 20.f, (float)pickOut.h, 13.f, g_outputFolder.empty() ? C_SUB : C_TEXT, false, -1);

        int y2 = y + 92;

        // fixed overlap: label and value separated
        DrawTextG(g, L"Result width (px)", (float)PAD, (float)y2, 200.f, 24.f, 14.f, C_SUB, false, -1);

        RectI wBox{ PAD + 210, y2 - 2, 120, 28 };
        SolidBrush wbg(Color(255, 28, 28, 28));
        g.FillRectangle(&wbg, wBox.x, wBox.y, wBox.w, wBox.h);
        g.DrawRectangle(&br, wBox.x, wBox.y, wBox.w, wBox.h);
        DrawTextG(g, std::to_wstring(g_outWidth), (float)wBox.x, (float)wBox.y, (float)wBox.w, (float)wBox.h, 14.f, C_TEXT, true, 0);

        RectI wMinus{ wBox.x + wBox.w + 10, wBox.y, 30, 28 };
        RectI wPlus{ wMinus.x + 38, wBox.y, 30, 28 };
        DrawButton(g, wMinus, L"-", false, false);
        DrawButton(g, wPlus, L"+", false, false);

        int y3 = y2 + 52;
        DrawTextG(g, L"Mode", (float)PAD, (float)y3, (float)w, 24.f, 14.f, C_SUB, false, -1);

        RectI m1{ PAD, y3 + 28, 270, 32 };
        RectI m2{ PAD, y3 + 72, 270, 32 };
        RectI m3{ PAD, y3 + 116, 270, 32 };

        RectI i1{ PAD + 292, y3 + 32, 18, 18 };
        RectI i2{ PAD + 292, y3 + 76, 18, 18 };
        RectI i3{ PAD + 292, y3 + 120, 18, 18 };

        auto drawMode = [&](const RectI& r, const wchar_t* label, DenoiseMode m)
            {
                bool active = (g_dnMode == m);
                SolidBrush bb(active ? Color(255, 36, 36, 36) : Color(255, 28, 28, 28));
                Pen bbr(active ? C_ACC : C_BORDER, active ? 2.f : 1.f);
                g.FillRectangle(&bb, r.x, r.y, r.w, r.h);
                g.DrawRectangle(&bbr, r.x, r.y, r.w, r.h);
                DrawTextG(g, label, (float)r.x + 12.f, (float)r.y, (float)r.w - 24.f, (float)r.h, 14.f, active ? C_TEXT : C_SUB, active, -1);
            };

        drawMode(m1, L"Manga", DenoiseMode::Manga);
        drawMode(m2, L"Color", DenoiseMode::Color);
        drawMode(m3, L"Balanced", DenoiseMode::Balanced);

        DrawInfoIcon(g, i1);
        DrawInfoIcon(g, i2);
        DrawInfoIcon(g, i3);

        bool canStart = !g_outputFolder.empty() && !g_inputs.empty();
        DrawFooter(g, w, h, true, true, L"Start", !canStart);
    }
    else if (g_tool == Tool::Merge)
    {
        DrawTextG(g, L"Output folder", (float)PAD, (float)y, (float)w, 24.f, 14.f, C_SUB, false, -1);

        RectI pickFolder{ PAD, y + 28, 420, 38 };
        g.FillRectangle(&box, pickFolder.x, pickFolder.y, pickFolder.w, pickFolder.h);
        g.DrawRectangle(&br, pickFolder.x, pickFolder.y, pickFolder.w, pickFolder.h);
        Pen acc(C_ACC, 2.0f);
        g.DrawLine(&acc, pickFolder.x, pickFolder.y + pickFolder.h - 1, pickFolder.x + pickFolder.w, pickFolder.y + pickFolder.h - 1);

        std::wstring outLine = g_mergeOutFolder.empty() ? L"Choose output folder..." : EllipsizePath(g_mergeOutFolder, 70);
        DrawTextG(g, outLine, (float)pickFolder.x + 10.f, (float)pickFolder.y, (float)pickFolder.w - 20.f, (float)pickFolder.h, 13.f,
            g_mergeOutFolder.empty() ? C_SUB : C_TEXT, false, -1);

        int y2 = y + 92;
        DrawTextG(g, L"File name", (float)PAD, (float)y2, 120.f, 24.f, 14.f, C_SUB, false, -1);

        RectI nBox{ PAD + 120, y2 - 2, 260, 28 };
        g.FillRectangle(&box, nBox.x, nBox.y, nBox.w, nBox.h);
        g.DrawRectangle(&br, nBox.x, nBox.y, nBox.w, nBox.h);

        std::wstring nameShown = (g_editField == EditField::MergeName) ? FieldTextWithCaret(EditField::MergeName, g_mergeName) : g_mergeName;
        if (g_editField == EditField::MergeName) nameShown = FieldTextWithCaret(EditField::MergeName, g_mergeName);
        DrawTextG(g, (g_editField == EditField::MergeName ? nameShown : g_mergeName),
            (float)nBox.x + 10.f, (float)nBox.y, (float)nBox.w - 20.f, (float)nBox.h, 14.f, C_TEXT, false, -1);

        int fy = y2 + 44;
        DrawTextG(g, L"Format", (float)PAD, (float)fy, 120.f, 24.f, 14.f, C_SUB, false, -1);

        RectI fPng{ PAD + 120, fy - 2, 100, 28 };
        RectI fJpg{ PAD + 230, fy - 2, 110, 28 };

        auto drawFmt = [&](const RectI& r, const wchar_t* label, bool active)
            {
                SolidBrush bb(active ? Color(255, 36, 36, 36) : Color(255, 28, 28, 28));
                Pen bbr(active ? C_ACC : C_BORDER, active ? 2.f : 1.f);
                g.FillRectangle(&bb, r.x, r.y, r.w, r.h);
                g.DrawRectangle(&bbr, r.x, r.y, r.w, r.h);
                DrawTextG(g, label, (float)r.x, (float)r.y, (float)r.w, (float)r.h, 13.f, active ? C_TEXT : C_SUB, active, 0);
            };

        drawFmt(fPng, L"PNG", !g_mergeJpeg);
        drawFmt(fJpg, L"JPEG", g_mergeJpeg);

        std::wstring preview = L"Will save: " + g_mergeName + (g_mergeJpeg ? L".jpg" : L".png");
        DrawTextG(g, preview, (float)PAD, (float)(fy + 34), (float)w - PAD * 2, 22.f, 13.f, C_SUB, false, -1);

        bool canStart = !g_mergeOutFolder.empty() && !g_inputs.empty();
        DrawFooter(g, w, h, true, true, L"Start", !canStart);
    }
    else // Rename
    {
        DrawTextG(g, L"Output folder", (float)PAD, (float)y, (float)w, 24.f, 14.f, C_SUB, false, -1);

        RectI pickOut{ PAD, y + 28, 420, 38 };
        g.FillRectangle(&box, pickOut.x, pickOut.y, pickOut.w, pickOut.h);
        g.DrawRectangle(&br, pickOut.x, pickOut.y, pickOut.w, pickOut.h);
        Pen acc(C_ACC, 2.0f);
        g.DrawLine(&acc, pickOut.x, pickOut.y + pickOut.h - 1, pickOut.x + pickOut.w, pickOut.y + pickOut.h - 1);

        std::wstring outLine = g_outputFolder.empty() ? L"Choose output folder..." : EllipsizePath(g_outputFolder, 70);
        DrawTextG(g, outLine, (float)pickOut.x + 10.f, (float)pickOut.y, (float)pickOut.w - 20.f, (float)pickOut.h, 13.f, g_outputFolder.empty() ? C_SUB : C_TEXT, false, -1);

        int y2 = y + 92;

        DrawTextG(g, L"Prefix", (float)PAD, (float)y2, 120.f, 24.f, 14.f, C_SUB, false, -1);
        RectI pBox{ PAD + 120, y2 - 2, 260, 28 };
        g.FillRectangle(&box, pBox.x, pBox.y, pBox.w, pBox.h);
        g.DrawRectangle(&br, pBox.x, pBox.y, pBox.w, pBox.h);

        std::wstring pShown = (g_editField == EditField::RenPrefix) ? FieldTextWithCaret(EditField::RenPrefix, g_renPrefix) : g_renPrefix;
        if (g_editField == EditField::RenPrefix) pShown = g_editBuf + L"|";
        DrawTextG(g, pShown, (float)pBox.x + 10.f, (float)pBox.y, (float)pBox.w - 20.f, (float)pBox.h, 14.f, C_TEXT, false, -1);

        int y3 = y2 + 40;
        DrawTextG(g, L"Suffix", (float)PAD, (float)y3, 120.f, 24.f, 14.f, C_SUB, false, -1);
        RectI sBox{ PAD + 120, y3 - 2, 260, 28 };
        g.FillRectangle(&box, sBox.x, sBox.y, sBox.w, sBox.h);
        g.DrawRectangle(&br, sBox.x, sBox.y, sBox.w, sBox.h);

        std::wstring sShown = (g_editField == EditField::RenSuffix) ? (g_editBuf + L"|") : g_renSuffix;
        DrawTextG(g, sShown, (float)sBox.x + 10.f, (float)sBox.y, (float)sBox.w - 20.f, (float)sBox.h, 14.f, C_TEXT, false, -1);

        int y4 = y3 + 46;
        DrawTextG(g, L"Start number", (float)PAD, (float)y4, 160.f, 24.f, 14.f, C_SUB, false, -1);

        RectI nBox{ PAD + 160, y4 - 2, 120, 28 };
        SolidBrush wbg(Color(255, 28, 28, 28));
        g.FillRectangle(&wbg, nBox.x, nBox.y, nBox.w, nBox.h);
        g.DrawRectangle(&br, nBox.x, nBox.y, nBox.w, nBox.h);
        DrawTextG(g, std::to_wstring(g_renStart), (float)nBox.x, (float)nBox.y, (float)nBox.w, (float)nBox.h, 14.f, C_TEXT, true, 0);

        RectI nMinus{ nBox.x + nBox.w + 10, nBox.y, 30, 28 };
        RectI nPlus{ nMinus.x + 38, nBox.y, 30, 28 };
        DrawButton(g, nMinus, L"-", false, false);
        DrawButton(g, nPlus, L"+", false, false);

        int y5 = y4 + 46;
        DrawTextG(g, L"Zero padding", (float)PAD, (float)y5, 160.f, 24.f, 14.f, C_SUB, false, -1);

        RectI zBox{ PAD + 160, y5 - 2, 120, 28 };
        g.FillRectangle(&wbg, zBox.x, zBox.y, zBox.w, zBox.h);
        g.DrawRectangle(&br, zBox.x, zBox.y, zBox.w, zBox.h);
        DrawTextG(g, std::to_wstring(g_renPad), (float)zBox.x, (float)zBox.y, (float)zBox.w, (float)zBox.h, 14.f, C_TEXT, true, 0);

        RectI zMinus{ zBox.x + zBox.w + 10, zBox.y, 30, 28 };
        RectI zPlus{ zMinus.x + 38, zBox.y, 30, 28 };
        DrawButton(g, zMinus, L"-", false, false);
        DrawButton(g, zPlus, L"+", false, false);

        std::wstring example = g_renPrefix + PadNumber(g_renStart, g_renPad) + g_renSuffix + L".ext";
        DrawTextG(g, L"Example: " + example, (float)PAD, (float)(y5 + 40), (float)w - PAD * 2, 24.f, 13.f, C_SUB, false, -1);

        bool canStart = !g_outputFolder.empty() && !g_inputs.empty();
        DrawFooter(g, w, h, true, true, L"Start", !canStart);
    }

    DrawWindowAccent(g, w, h);
    DrawTooltip(g, w, h);
}

static void DrawProcessing(Graphics& g, const RECT& rc)
{
    int w = rc.right;
    int h = rc.bottom;

    SolidBrush bg(C_BG);
    g.FillRectangle(&bg, 0, 0, w, h);

    DrawToolbarCommon(g, w, true);

    std::wstring title = (g_tool == Tool::Denoise) ? L"Denoise" : (g_tool == Tool::Merge) ? L"Merge" : L"Rename";
    DrawTextG(g, title, (float)(PAD + 58), 0.f, (float)(w - PAD - 58), (float)TOOLBAR_H, 16.f, C_TEXT, true, -1);

    DrawTextG(g, L"Processing...", 0.f, (float)(TOOLBAR_H + 70), (float)w, 34.f, 24.f, C_TEXT, true, 0);

    int barX = PAD;
    int barY = TOOLBAR_H + 140;
    int barW = w - PAD * 2;
    int barH = 18;

    SolidBrush barBg(Color(255, 40, 40, 40));
    g.FillRectangle(&barBg, barX, barY, barW, barH);

    int total = max(1, g_total);
    int done = ClampI(g_processed, 0, total);
    float k = (float)done / (float)total;
    int fillW = (int)(barW * k);

    SolidBrush barFill(C_ACC);
    g.FillRectangle(&barFill, barX, barY, fillW, barH);

    Pen br(C_BORDER, 1.0f);
    g.DrawRectangle(&br, barX, barY, barW, barH);

    std::wstring line = L"Done: " + std::to_wstring(done) + L" / " + std::to_wstring(g_total);
    DrawTextG(g, line, 0.f, (float)(barY + 28), (float)w, 24.f, 14.f, C_SUB, false, 0);

    DrawFooter(g, w, h, true, false, L"", true);
    DrawWindowAccent(g, w, h);
}

static void DrawDone(Graphics& g, const RECT& rc)
{
    int w = rc.right;
    int h = rc.bottom;

    SolidBrush bg(C_BG);
    g.FillRectangle(&bg, 0, 0, w, h);

    DrawToolbarCommon(g, w, true);

    DrawTextG(g, L"Completed", 0.f, (float)(TOOLBAR_H + 40), (float)w, 44.f, 30.f, C_TEXT, true, 0);

    std::wstring timeLine = L"Total time: " + FormatDuration(g_elapsedMs);
    DrawTextG(g, timeLine, 0.f, (float)(TOOLBAR_H + 86), (float)w, 26.f, 14.f, C_SUB, false, 0);

    int cx = w / 2;
    int cy = TOOLBAR_H + 290;
    DrawDoneCat(g, cx, cy, g_animTick);

    RectI again{ w / 2 - 170, h - FOOTER_H - 70, 160, 40 };
    RectI home{ w / 2 + 10,  h - FOOTER_H - 70, 160, 40 };
    DrawButton(g, again, L"Process more", false);
    DrawButton(g, home, L"Home", false);

    DrawFooter(g, w, h, true, false, L"", true);
    DrawWindowAccent(g, w, h);
}

// ------------------------------
// Hit tests / clicks
// ------------------------------
static void CommitEdit()
{
    if (g_editField == EditField::None) return;

    if (g_editField == EditField::MergeName) g_mergeName = g_editBuf;
    if (g_editField == EditField::RenPrefix) g_renPrefix = g_editBuf;
    if (g_editField == EditField::RenSuffix) g_renSuffix = g_editBuf;

    g_editField = EditField::None;
    g_editBuf.clear();
}

static void CancelEdit()
{
    g_editField = EditField::None;
    g_editBuf.clear();
}

static void StartEdit(EditField f, const std::wstring& initial)
{
    g_editField = f;
    g_editBuf = initial;
}

static void HitHomeClick(HWND hWnd, int mx, int my, const RECT& rc)
{
    int w = rc.right;
    int bw = 180;
    int bx = (w - (bw * 3 + 24 * 2)) / 2;
    int by = 220;

    RectI b1{ bx, by, bw, 44 };
    RectI b2{ bx + bw + 24, by, bw, 44 };
    RectI b3{ bx + (bw + 24) * 2, by, bw, 44 };

    if (PtIn(b1, mx, my)) { g_tool = Tool::Denoise; ClearToolStateKeepTool(); g_view = View::Pick; DragAcceptFiles(hWnd, TRUE); InvalidateRect(hWnd, nullptr, TRUE); return; }
    if (PtIn(b2, mx, my)) { g_tool = Tool::Merge;   ClearToolStateKeepTool(); g_view = View::Pick; DragAcceptFiles(hWnd, TRUE); InvalidateRect(hWnd, nullptr, TRUE); return; }
    if (PtIn(b3, mx, my)) { g_tool = Tool::Rename;  ClearToolStateKeepTool(); g_view = View::Pick; DragAcceptFiles(hWnd, TRUE); InvalidateRect(hWnd, nullptr, TRUE); return; }
}

static void HandlePickClick(HWND hWnd, int mx, int my, const RECT& rc)
{
    RectI back{ PAD, 12, 42, 32 };
    RectI choose{ PAD + 58, 12, 160, 32 };

    if (my < TOOLBAR_H)
    {
        if (PtIn(back, mx, my))
        {
            ClearAllStateToHome();
            InvalidateRect(hWnd, nullptr, TRUE);
            return;
        }
        if (PtIn(choose, mx, my))
        {
            std::wstring folder = PickFolderDialog(hWnd, L"Select input folder");
            if (!folder.empty())
            {
                LoadInputsFromFolder(folder);
                InvalidateRect(hWnd, nullptr, TRUE);
            }
            return;
        }
    }

    RectI btnBack{ PAD, rc.bottom - FOOTER_H + 6, 120, 32 };
    RectI btnNext{ rc.right - PAD - 160, rc.bottom - FOOTER_H + 6, 160, 32 };

    if (my >= rc.bottom - FOOTER_H)
    {
        if (PtIn(btnBack, mx, my))
        {
            ClearAllStateToHome();
            InvalidateRect(hWnd, nullptr, TRUE);
            return;
        }
        if (PtIn(btnNext, mx, my) && !g_inputs.empty())
        {
            g_view = View::Setup;
            SetStatus(L"Configure");
            InvalidateRect(hWnd, nullptr, TRUE);
            return;
        }
    }
}

static void HandleSetupClick(HWND hWnd, int mx, int my, const RECT& rc)
{
    RectI backHome{ PAD, 12, 42, 32 };
    if (my < TOOLBAR_H && PtIn(backHome, mx, my))
    {
        CancelEdit();
        ClearAllStateToHome();
        InvalidateRect(hWnd, nullptr, TRUE);
        return;
    }

    RectI btnBack{ PAD, rc.bottom - FOOTER_H + 6, 120, 32 };
    RectI btnNext{ rc.right - PAD - 160, rc.bottom - FOOTER_H + 6, 160, 32 };

    if (my >= rc.bottom - FOOTER_H)
    {
        if (PtIn(btnBack, mx, my))
        {
            CommitEdit();
            g_view = View::Pick;
            SetStatus(L"Ready");
            InvalidateRect(hWnd, nullptr, TRUE);
            return;
        }

        if (PtIn(btnNext, mx, my))
        {
            CommitEdit();
            // validate and start
            if (g_tool == Tool::Denoise || g_tool == Tool::Rename)
            {
                if (g_outputFolder.empty()) { SetStatus(L"Select output folder first"); InvalidateRect(hWnd, nullptr, TRUE); return; }
                StartWorker(hWnd);
                InvalidateRect(hWnd, nullptr, TRUE);
                return;
            }
            if (g_tool == Tool::Merge)
            {
                if (g_mergeOutFolder.empty()) { SetStatus(L"Select output folder first"); InvalidateRect(hWnd, nullptr, TRUE); return; }
                StartWorker(hWnd);
                InvalidateRect(hWnd, nullptr, TRUE);
                return;
            }
        }
    }

    // Clicking outside edit fields commits edit
    if (g_editField != EditField::None)
    {
        // do not auto-commit if click is still inside the active box; handled below
    }

    if (g_tool == Tool::Denoise)
    {
        RectI pickOut{ PAD, TOOLBAR_H + 56, 420, 38 };
        if (PtIn(pickOut, mx, my))
        {
            CommitEdit();
            std::wstring folder = PickFolderDialog(hWnd, L"Select output folder");
            if (!folder.empty())
            {
                g_outputFolder = folder;
                SetStatus(L"Output folder selected");
                InvalidateRect(hWnd, nullptr, TRUE);
            }
            return;
        }

        int y2 = TOOLBAR_H + 28 + 92;
        RectI wBox{ PAD + 210, y2 - 2, 120, 28 };
        RectI wMinus{ wBox.x + wBox.w + 10, wBox.y, 30, 28 };
        RectI wPlus{ wMinus.x + 38, wBox.y, 30, 28 };

        if (PtIn(wMinus, mx, my))
        {
            g_outWidth = ClampI(g_outWidth - 100, 300, 6000);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
        if (PtIn(wPlus, mx, my))
        {
            g_outWidth = ClampI(g_outWidth + 100, 300, 6000);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }

        int y3 = y2 + 52;
        RectI m1{ PAD, y3 + 28, 270, 32 };
        RectI m2{ PAD, y3 + 72, 270, 32 };
        RectI m3{ PAD, y3 + 116, 270, 32 };

        if (PtIn(m1, mx, my)) { g_dnMode = DenoiseMode::Manga; InvalidateRect(hWnd, nullptr, FALSE); return; }
        if (PtIn(m2, mx, my)) { g_dnMode = DenoiseMode::Color; InvalidateRect(hWnd, nullptr, FALSE); return; }
        if (PtIn(m3, mx, my)) { g_dnMode = DenoiseMode::Balanced; InvalidateRect(hWnd, nullptr, FALSE); return; }

        // click elsewhere -> hide tooltip and commit edit (no edit here)
        CommitEdit();
    }
    else if (g_tool == Tool::Merge)
    {
        int y = TOOLBAR_H + 28;

        RectI pickFolder{ PAD, y + 28, 420, 38 };
        if (PtIn(pickFolder, mx, my))
        {
            CommitEdit();
            std::wstring folder = PickFolderDialog(hWnd, L"Select output folder");
            if (!folder.empty())
            {
                g_mergeOutFolder = folder;
                SetStatus(L"Output folder selected");
                InvalidateRect(hWnd, nullptr, TRUE);
            }
            return;
        }

        int y2 = y + 92;
        RectI nameBox{ PAD + 120, y2 - 2, 260, 28 };
        if (PtIn(nameBox, mx, my))
        {
            if (g_editField != EditField::MergeName)
                StartEdit(EditField::MergeName, g_mergeName);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }

        int fy = y2 + 44;
        RectI fPng{ PAD + 120, fy - 2, 100, 28 };
        RectI fJpg{ PAD + 230, fy - 2, 110, 28 };

        if (PtIn(fPng, mx, my)) { CommitEdit(); g_mergeJpeg = false; InvalidateRect(hWnd, nullptr, FALSE); return; }
        if (PtIn(fJpg, mx, my)) { CommitEdit(); g_mergeJpeg = true;  InvalidateRect(hWnd, nullptr, FALSE); return; }

        // click elsewhere commits edit
        CommitEdit();
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    else if (g_tool == Tool::Rename)
    {
        RectI pickOut{ PAD, TOOLBAR_H + 56, 420, 38 };
        if (PtIn(pickOut, mx, my))
        {
            CommitEdit();
            std::wstring folder = PickFolderDialog(hWnd, L"Select output folder");
            if (!folder.empty())
            {
                g_outputFolder = folder;
                SetStatus(L"Output folder selected");
                InvalidateRect(hWnd, nullptr, TRUE);
            }
            return;
        }

        int yBase = TOOLBAR_H + 28 + 92;
        RectI pBox{ PAD + 120, yBase - 2, 260, 28 };
        RectI sBox{ PAD + 120, yBase + 38, 260, 28 };

        if (PtIn(pBox, mx, my))
        {
            if (g_editField != EditField::RenPrefix)
                StartEdit(EditField::RenPrefix, g_renPrefix);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
        if (PtIn(sBox, mx, my))
        {
            if (g_editField != EditField::RenSuffix)
                StartEdit(EditField::RenSuffix, g_renSuffix);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }

        int y4 = yBase + 84;
        RectI nBox{ PAD + 160, y4 - 2, 120, 28 };
        RectI nMinus{ nBox.x + nBox.w + 10, nBox.y, 30, 28 };
        RectI nPlus{ nMinus.x + 38, nBox.y, 30, 28 };

        if (PtIn(nMinus, mx, my))
        {
            CommitEdit();
            g_renStart = max(0, g_renStart - 1);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
        if (PtIn(nPlus, mx, my))
        {
            CommitEdit();
            g_renStart = min(999999, g_renStart + 1);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }

        int y5 = y4 + 46;
        RectI zBox{ PAD + 160, y5 - 2, 120, 28 };
        RectI zMinus{ zBox.x + zBox.w + 10, zBox.y, 30, 28 };
        RectI zPlus{ zMinus.x + 38, zBox.y, 30, 28 };

        if (PtIn(zMinus, mx, my))
        {
            CommitEdit();
            g_renPad = ClampI(g_renPad - 1, 0, 6);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
        if (PtIn(zPlus, mx, my))
        {
            CommitEdit();
            g_renPad = ClampI(g_renPad + 1, 0, 6);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }

        // click elsewhere commits edit
        CommitEdit();
        InvalidateRect(hWnd, nullptr, FALSE);
    }
}

static void HandleDoneClick(HWND hWnd, int mx, int my, const RECT& rc)
{
    RectI btnBack{ PAD, rc.bottom - FOOTER_H + 6, 120, 32 };
    if (my >= rc.bottom - FOOTER_H && PtIn(btnBack, mx, my))
    {
        g_view = View::Pick;
        ClearToolStateKeepTool();
        SetStatus(L"Ready");
        InvalidateRect(hWnd, nullptr, TRUE);
        return;
    }

    RectI again{ rc.right / 2 - 170, rc.bottom - FOOTER_H - 70, 160, 40 };
    RectI home{ rc.right / 2 + 10,  rc.bottom - FOOTER_H - 70, 160, 40 };

    if (PtIn(again, mx, my))
    {
        g_view = View::Pick;
        ClearToolStateKeepTool();
        SetStatus(L"Ready");
        InvalidateRect(hWnd, nullptr, TRUE);
        return;
    }
    if (PtIn(home, mx, my))
    {
        ClearAllStateToHome();
        InvalidateRect(hWnd, nullptr, TRUE);
        return;
    }
}

// ------------------------------
// Keyboard editing
// ------------------------------
static bool IsAllowedChar(wchar_t c)
{
    if (c >= 32 && c != L'\t' && c != L'\r' && c != L'\n') return true;
    return false;
}

static void HandleCharInput(HWND hWnd, wchar_t ch)
{
    if (g_editField == EditField::None) return;

    if (ch == 8) // backspace
    {
        if (!g_editBuf.empty()) g_editBuf.pop_back();
        InvalidateRect(hWnd, nullptr, FALSE);
        return;
    }
    if (ch == 13) // enter
    {
        CommitEdit();
        InvalidateRect(hWnd, nullptr, FALSE);
        return;
    }
    if (ch == 27) // esc
    {
        CancelEdit();
        InvalidateRect(hWnd, nullptr, FALSE);
        return;
    }

    if (IsAllowedChar(ch))
    {
        if ((int)g_editBuf.size() < 200)
            g_editBuf.push_back(ch);
        InvalidateRect(hWnd, nullptr, FALSE);
    }
}

// ------------------------------
// Window proc
// ------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        g_hWndMain = hWnd;
        DragAcceptFiles(hWnd, TRUE);
        SetTimer(hWnd, TIMER_UI, 16, nullptr);
        SetTimer(hWnd, TIMER_ANIM, 50, nullptr);
        return 0;

    case WM_DROPFILES:
    {
        if (g_view == View::Home) return 0;
        HDROP hDrop = (HDROP)wParam;
        LoadInputsFromDrop(hDrop);
        DragFinish(hDrop);
        InvalidateRect(hWnd, nullptr, TRUE);
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        if (g_view == View::Pick && !g_inputs.empty())
        {
            short d = GET_WHEEL_DELTA_WPARAM(wParam);
            g_scrollTarget -= (d / 120) * 160;
            RECT rc; GetClientRect(hWnd, &rc);
            ComputeScrollMax(rc);
            g_scrollTarget = ClampI(g_scrollTarget, 0, g_scrollMax);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        UpdateTooltipByMouse(mx, my);
        return 0;
    }

    case WM_CHAR:
        HandleCharInput(hWnd, (wchar_t)wParam);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE && g_editField != EditField::None) { CancelEdit(); InvalidateRect(hWnd, nullptr, FALSE); return 0; }
        if (wParam == VK_RETURN && g_editField != EditField::None) { CommitEdit(); InvalidateRect(hWnd, nullptr, FALSE); return 0; }
        return 0;

    case WM_LBUTTONDOWN:
    {
        RECT rc; GetClientRect(hWnd, &rc);
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);

        if (g_view == View::Home) { HitHomeClick(hWnd, mx, my, rc); return 0; }
        if (g_view == View::Pick) { HandlePickClick(hWnd, mx, my, rc); return 0; }
        if (g_view == View::Setup) { HandleSetupClick(hWnd, mx, my, rc); return 0; }
        if (g_view == View::Done) { HandleDoneClick(hWnd, mx, my, rc); return 0; }
        return 0;
    }

    case WM_TIMER:
        if (wParam == TIMER_UI)
        {
            if (g_view == View::Pick)
            {
                RECT rc; GetClientRect(hWnd, &rc);
                ComputeScrollMax(rc);

                int diff = g_scrollTarget - g_scrollY;
                if (diff != 0)
                {
                    g_scrollY += diff / 4;
                    if (iabs(diff) < 2) g_scrollY = g_scrollTarget;
                    g_scrollY = ClampI(g_scrollY, 0, g_scrollMax);
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
            }
        }
        else if (wParam == TIMER_ANIM)
        {
            g_animTick++;
            if (g_view == View::Home || g_view == View::Done) InvalidateRect(hWnd, nullptr, FALSE);
            if (g_ttShow) InvalidateRect(hWnd, nullptr, FALSE);
            if (g_editField != EditField::None) InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;

    case WM_APP_PROGRESS:
        g_processed = (int)wParam;
        g_total = (int)lParam;
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;

    case WM_APP_DONE:
        g_processing = false;
        g_elapsedMs = (long long)lParam;
        StopWorkerIfAny();
        SetStatus(L"Done");
        g_view = View::Done;
        InvalidateRect(hWnd, nullptr, TRUE);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rc; GetClientRect(hWnd, &rc);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

        Graphics g(memDC);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.Clear(C_BG);

        if (g_view == View::Home) DrawHome(g, rc);
        else if (g_view == View::Pick) DrawPick(g, rc);
        else if (g_view == View::Setup) DrawSetup(g, rc);
        else if (g_view == View::Processing) DrawProcessing(g, rc);
        else DrawDone(g, rc);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        StopWorkerIfAny();
        KillTimer(hWnd, TIMER_UI);
        KillTimer(hWnd, TIMER_ANIM);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ------------------------------
// Entry
// ------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

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
        1200, 820,
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
