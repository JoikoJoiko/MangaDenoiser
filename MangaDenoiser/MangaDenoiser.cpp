#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <algorithm>
#include <memory>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "Shell32.lib")

using namespace Gdiplus;

constexpr wchar_t WINDOW_CLASS[] = L"MangaDenoiserWindow";
constexpr wchar_t WINDOW_TITLE[] = L"Manga Denoiser";

constexpr int TOOLBAR_H = 56;
constexpr int FOOTER_H = 40;

constexpr int PAD = 16;
constexpr int CELL = 140;
constexpr int GAP = 16;

constexpr int BTN_W = 148;
constexpr int BTN_H = 32;

constexpr UINT_PTR TIMER_SCROLL = 1;
constexpr UINT_PTR TIMER_PROCESS = 2;
constexpr UINT_PTR TIMER_DONE_ANIM = 3;

struct RectI { int x, y, w, h; };

static inline bool PtIn(const RectI& r, int px, int py) { return px >= r.x && px < (r.x + r.w) && py >= r.y && py < (r.y + r.h); }
static inline int ClampI(int v, int a, int b) { return (v < a) ? a : (v > b) ? b : v; }

enum class Screen { Gallery, Processing, Done };

ULONG_PTR g_gdiplusToken = 0;

Screen g_screen = Screen::Gallery;

std::vector<std::wstring> g_images;
std::vector<std::unique_ptr<Bitmap>> g_thumbs;

std::wstring g_inputFolder;
std::wstring g_outputFolder;

std::wstring g_status = L"Ready";

int g_scrollY = 0;
int g_scrollTarget = 0;
int g_scrollMax = 0;

bool g_processing = false;
int  g_processed = 0;

int g_doneTick = 0;

static Color C_BG(255, 24, 24, 24);
static Color C_PANEL(255, 32, 32, 32);
static Color C_BTN(255, 55, 55, 55);
static Color C_BTN_DIS(255, 40, 40, 40);
static Color C_BORDER(255, 90, 90, 90);
static Color C_TEXT(255, 220, 220, 220);
static Color C_SUB(255, 170, 170, 170);
static Color C_ACC(255, 255, 120, 205);

static void ResetThumbs(size_t n)
{
    g_thumbs.clear();
    g_thumbs.resize(n);
}

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

    do
    {
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
    if (!SHGetPathFromIDListW(pidl, path))
    {
        CoTaskMemFree(pidl);
        return L"";
    }

    CoTaskMemFree(pidl);
    return path;
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

static void ResetGalleryAfterLoad()
{
    g_scrollY = 0;
    g_scrollTarget = 0;
    g_scrollMax = 0;
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

static std::wstring EllipsizePath(const std::wstring& s, int maxChars)
{
    if ((int)s.size() <= maxChars) return s;
    if (maxChars < 10) return s.substr(0, maxChars);
    int keepL = maxChars / 2 - 2;
    int keepR = maxChars - keepL - 3;
    return s.substr(0, keepL) + L"..." + s.substr((int)s.size() - keepR);
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

static void BuildRoundRectPath(GraphicsPath& path, const RectF& r, REAL radius)
{
    REAL rr = radius;
    if (rr < 0) rr = 0;
    REAL maxR = min(r.Width, r.Height) / 2.0f;
    if (rr > maxR) rr = maxR;

    REAL d = rr * 2.0f;
    REAL x = r.X;
    REAL y = r.Y;
    REAL w = r.Width;
    REAL h = r.Height;

    if (rr <= 0.0f)
    {
        path.AddRectangle(r);
        path.CloseFigure();
        return;
    }

    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

static void FillRoundedRect(Graphics& g, Brush& brush, const RectF& r, REAL radius)
{
    GraphicsPath path;
    BuildRoundRectPath(path, r, radius);
    g.FillPath(&brush, &path);
}

static void DrawRoundedRect(Graphics& g, Pen& pen, const RectF& r, REAL radius)
{
    GraphicsPath path;
    BuildRoundRectPath(path, r, radius);
    g.DrawPath(&pen, &path);
}

static void DrawButton(Graphics& g, const RectI& r, const wchar_t* text, bool disabled)
{
    SolidBrush bg(disabled ? C_BTN_DIS : C_BTN);
    Pen br(C_BORDER, 1.0f);

    g.FillRectangle(&bg, r.x, r.y, r.w, r.h);
    g.DrawRectangle(&br, r.x, r.y, r.w, r.h);

    if (!disabled)
    {
        Pen acc(C_ACC, 2.0f);
        g.DrawLine(&acc, r.x + 2, r.y + r.h - 1, r.x + r.w - 2, r.y + r.h - 1);
    }

    DrawTextG(g, text, (float)r.x, (float)r.y, (float)r.w, (float)r.h, 14.0f, disabled ? Color(255, 130, 130, 130) : C_TEXT, false, 0);
}

static void DrawPanelOutline(Graphics& g, int w, int h)
{
    Pen acc(C_ACC, 2.0f);
    g.DrawRectangle(&acc, 1, 1, w - 3, h - 3);
}

static void ComputeScroll(const RECT& rc)
{
    if (g_screen != Screen::Gallery)
    {
        g_scrollY = 0;
        g_scrollTarget = 0;
        g_scrollMax = 0;
        return;
    }

    int viewH = (rc.bottom - rc.top) - TOOLBAR_H - FOOTER_H;
    int cols = ComputeCols(rc.right);
    int contentH = ComputeContentHeight(cols);

    g_scrollMax = max(0, contentH - viewH);
    g_scrollTarget = ClampI(g_scrollTarget, 0, g_scrollMax);
    g_scrollY = ClampI(g_scrollY, 0, g_scrollMax);
}

static void StartProcessing(HWND hWnd)
{
    if (g_images.empty()) return;
    if (g_outputFolder.empty()) return;

    g_processing = true;
    g_processed = 0;
    g_status = L"Processing...";
    KillTimer(hWnd, TIMER_PROCESS);
    SetTimer(hWnd, TIMER_PROCESS, 25, nullptr);
}

static void StopProcessing(HWND hWnd)
{
    g_processing = false;
    KillTimer(hWnd, TIMER_PROCESS);
}

static void SwitchToDone(HWND hWnd)
{
    g_screen = Screen::Done;
    g_doneTick = 0;
    g_status = L"Done";
    KillTimer(hWnd, TIMER_DONE_ANIM);
    SetTimer(hWnd, TIMER_DONE_ANIM, 50, nullptr);
}

static void DrawCat(Graphics& g, int cx, int cy, int t)
{
    int sway = (t % 20) - 10;

    SolidBrush body(Color(255, 50, 50, 50));
    SolidBrush face(Color(255, 60, 60, 60));
    SolidBrush eye(Color(255, 230, 230, 230));
    SolidBrush blush(C_ACC);

    Pen outline(Color(255, 90, 90, 90), 2.0f);
    Pen whisk(C_SUB, 2.0f);
    Pen acc(C_ACC, 2.0f);

    int bw = 210;
    int bh = 150;

    Rect bodyR(cx - bw / 2, cy - bh / 2 + 50, bw, bh);
    FillRoundedRect(g, body, RectF((REAL)bodyR.X, (REAL)bodyR.Y, (REAL)bodyR.Width, (REAL)bodyR.Height), 22.0f);

    Rect headR(cx - 120, cy - 120, 240, 200);
    FillRoundedRect(g, face, RectF((REAL)headR.X, (REAL)headR.Y, (REAL)headR.Width, (REAL)headR.Height), 36.0f);
    DrawRoundedRect(g, outline, RectF((REAL)headR.X, (REAL)headR.Y, (REAL)headR.Width, (REAL)headR.Height), 36.0f);

    Point earL[3] = { Point(cx - 85, cy - 120), Point(cx - 130, cy - 170), Point(cx - 40, cy - 150) };
    Point earR[3] = { Point(cx + 85, cy - 120), Point(cx + 130, cy - 170), Point(cx + 40, cy - 150) };
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

    int tailX = cx + bw / 2 - 10;
    int tailY = cy + 90;
    int tailW = 90;
    int tailH = 30;

    float phase = (float)(t % 100) / 100.0f;
    float s = sinf(phase * 6.2831853f);          
    float a = s * 18.0f;                         

    PointF p0((REAL)(cx + bw / 2 - 18), (REAL)(cy + 90));        
    PointF p3((REAL)(cx + bw / 2 + 55), (REAL)(cy + 70));        
    p3.X += a;                                                   
    p3.Y += (-fabsf(s) * 6.0f);                                 

    PointF c1(p0.X + 25.0f, p0.Y - 5.0f);                        
    PointF c2(p3.X - 25.0f, p3.Y + 10.0f);                      
    c2.Y += s * 8.0f;                                            

    GraphicsPath tail;
    tail.AddBezier(p0, c1, c2, p3);
    g.DrawPath(&acc, &tail);
}

static void LoadImagesFromPaths(const std::vector<std::wstring>& collected, const std::wstring& inferredFolder)
{
    g_images = collected;
    std::sort(g_images.begin(), g_images.end());
    ResetThumbs(g_images.size());

    g_inputFolder = inferredFolder;
    ResetGalleryAfterLoad();
    g_screen = Screen::Gallery;
    g_processing = false;
    g_processed = 0;
    g_status = g_images.empty() ? L"No images found" : L"Images loaded";
}

static POINT GetClientPtFromWheel(HWND hWnd, LPARAM lParam)
{
    POINT p;
    p.x = GET_X_LPARAM(lParam);
    p.y = GET_Y_LPARAM(lParam);
    ScreenToClient(hWnd, &p);
    return p;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        DragAcceptFiles(hWnd, TRUE);
        SetTimer(hWnd, TIMER_SCROLL, 16, nullptr);
        return 0;

    case WM_DROPFILES:
    {
        HDROP hDrop = (HDROP)wParam;

        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        wchar_t path[MAX_PATH]{};

        std::vector<std::wstring> collected;
        std::wstring folderHint;

        for (UINT i = 0; i < count; i++)
        {
            if (!DragQueryFileW(hDrop, i, path, MAX_PATH)) continue;
            std::wstring p = path;

            DWORD attr = GetFileAttributesW(p.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES) continue;

            if (attr & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (folderHint.empty()) folderHint = p;
                AddImagesFromFolder(p, collected);
            }
            else
            {
                if (IsImageFile(p)) collected.push_back(p);
            }
        }

        DragFinish(hDrop);

        if (!collected.empty())
        {
            if (folderHint.empty())
            {
                auto pos = collected[0].find_last_of(L"\\/");
                folderHint = (pos == std::wstring::npos) ? L"" : collected[0].substr(0, pos);
            }
            LoadImagesFromPaths(collected, folderHint);
        }
        else
        {
            g_images.clear();
            ResetThumbs(0);
            g_inputFolder.clear();
            g_status = L"No images found";
            ResetGalleryAfterLoad();
            g_screen = Screen::Gallery;
        }

        InvalidateRect(hWnd, nullptr, TRUE);
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        if (g_screen != Screen::Gallery) return 0;

        RECT rc; GetClientRect(hWnd, &rc);
        POINT pt = GetClientPtFromWheel(hWnd, lParam);

        int viewTop = TOOLBAR_H;
        int viewBottom = rc.bottom - FOOTER_H;

        if (pt.y < viewTop || pt.y > viewBottom) return 0;

        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        g_scrollTarget -= (delta / 120) * 140;
        g_scrollTarget = ClampI(g_scrollTarget, 0, g_scrollMax);

        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }

    case WM_TIMER:
        if (wParam == TIMER_SCROLL)
        {
            RECT rc; GetClientRect(hWnd, &rc);
            ComputeScroll(rc);

            int diff = g_scrollTarget - g_scrollY;
            g_scrollY += diff / 4;
            if (abs(diff) < 2) g_scrollY = g_scrollTarget;
            g_scrollY = ClampI(g_scrollY, 0, g_scrollMax);

            InvalidateRect(hWnd, nullptr, FALSE);
        }
        else if (wParam == TIMER_PROCESS)
        {
            if (!g_processing) return 0;

            g_processed++;
            if (g_processed >= (int)g_images.size())
            {
                StopProcessing(hWnd);
                SwitchToDone(hWnd);
            }

            InvalidateRect(hWnd, nullptr, FALSE);
        }
        else if (wParam == TIMER_DONE_ANIM)
        {
            g_doneTick++;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN:
    {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);

        RECT rc; GetClientRect(hWnd, &rc);

        RectI btnAdd{ PAD, 12, BTN_W, BTN_H };
        RectI btnNext{ PAD + BTN_W + 12, 12, BTN_W, BTN_H };
        RectI btnBack{ PAD, rc.bottom - FOOTER_H + 6, 110, 28 };

        if (my < TOOLBAR_H)
        {
            if (PtIn(btnAdd, mx, my))
            {
                std::wstring picked = PickFolderDialog(hWnd, L"Select folder with images");
                if (!picked.empty())
                {
                    std::vector<std::wstring> collected;
                    AddImagesFromFolder(picked, collected);
                    LoadImagesFromPaths(collected, picked);
                    InvalidateRect(hWnd, nullptr, TRUE);
                }
                return 0;
            }

            if (PtIn(btnNext, mx, my))
            {
                if (g_screen == Screen::Gallery && !g_images.empty())
                {
                    g_screen = Screen::Processing;
                    g_status = L"Choose output folder";
                    InvalidateRect(hWnd, nullptr, TRUE);
                }
                return 0;
            }
        }

        if (my >= (rc.bottom - FOOTER_H))
        {
            if (g_screen != Screen::Gallery && PtIn(btnBack, mx, my))
            {
                if (g_screen == Screen::Processing) StopProcessing(hWnd);
                g_screen = Screen::Gallery;
                g_status = L"Ready";
                InvalidateRect(hWnd, nullptr, TRUE);
                return 0;
            }
        }

        if (g_screen == Screen::Processing)
        {
            RectI outPick{ PAD, TOOLBAR_H + 90, 420, 38 };
            RectI startBtn{ PAD, TOOLBAR_H + 150, 160, 36 };

            if (PtIn(outPick, mx, my))
            {
                std::wstring picked = PickFolderDialog(hWnd, L"Select output folder");
                if (!picked.empty())
                {
                    g_outputFolder = picked;
                    g_status = L"Output folder selected";
                    InvalidateRect(hWnd, nullptr, TRUE);
                }
                return 0;
            }

            bool canStart = !g_outputFolder.empty() && !g_images.empty() && !g_processing;
            if (PtIn(startBtn, mx, my))
            {
                if (!canStart)
                {
                    g_status = g_outputFolder.empty() ? L"Select output folder first" : L"No images to process";
                    InvalidateRect(hWnd, nullptr, TRUE);
                    return 0;
                }
                StartProcessing(hWnd);
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

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

        Graphics g(memDC);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.Clear(C_BG);

        SolidBrush toolbar(C_PANEL);
        g.FillRectangle(&toolbar, 0, 0, rc.right, TOOLBAR_H);

        Pen acc(C_ACC, 2.0f);
        g.DrawLine(&acc, 0, TOOLBAR_H - 1, rc.right, TOOLBAR_H - 1);

        RectI btnAdd{ PAD, 12, BTN_W, BTN_H };
        RectI btnNext{ PAD + BTN_W + 12, 12, BTN_W, BTN_H };

        bool nextDisabled = g_images.empty();
        DrawButton(g, btnAdd, L"Add folder", false);
        DrawButton(g, btnNext, L"Next", nextDisabled);

        std::wstring folderLine = g_inputFolder.empty() ? L"Drop a folder/images to start" : g_inputFolder;
        folderLine = EllipsizePath(folderLine, 110);

        int pathX = PAD + (BTN_W + 12) * 2;
        int pathW = max(0, rc.right - PAD - pathX);
        DrawTextG(g, folderLine, (float)pathX, 0.f, (float)pathW, (float)TOOLBAR_H, 13.0f, C_SUB, false, -1);

        SolidBrush footer(C_PANEL);
        g.FillRectangle(&footer, 0, rc.bottom - FOOTER_H, rc.right, FOOTER_H);
        g.DrawLine(&acc, 0, rc.bottom - FOOTER_H, rc.right, rc.bottom - FOOTER_H);

        RectI btnBack{ PAD, rc.bottom - FOOTER_H + 6, 110, 28 };
        bool showBack = (g_screen != Screen::Gallery);
        if (showBack) DrawButton(g, btnBack, L"Back", false);

        int statusX = PAD;
        if (showBack) statusX = btnBack.x + btnBack.w + 24;

        std::wstring footerTxt = L"Images: " + std::to_wstring(g_images.size()) + L"    Status: " + g_status;
        DrawTextG(g, footerTxt, (float)statusX, (float)(rc.bottom - FOOTER_H), (float)(rc.right - statusX - PAD), (float)FOOTER_H, 13.0f, C_SUB, false, -1);

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
                int y0 = viewTop + PAD - g_scrollY;
                int step = CELL + GAP;

                int firstRow = max(0, (g_scrollY - PAD) / step);
                int rowsVisible = (viewH / step) + 3;

                int totalRows = (int)((g_images.size() + cols - 1) / cols);
                int lastRow = min(totalRows - 1, firstRow + rowsVisible);

                for (int row = firstRow; row <= lastRow; row++)
                {
                    for (int col = 0; col < cols; col++)
                    {
                        size_t i = (size_t)row * (size_t)cols + (size_t)col;
                        if (i >= g_images.size()) break;

                        int x = PAD + col * step;
                        int y = y0 + row * step;

                        EnsureThumb(i);
                        Rect r(x, y, CELL, CELL);

                        if (g_thumbs[i] && g_thumbs[i]->GetLastStatus() == Ok)
                        {
                            g.DrawImage(g_thumbs[i].get(), r);
                        }
                        else
                        {
                            SolidBrush ph(Color(255, 18, 18, 18));
                            Pen br(C_BORDER, 1.0f);
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
            DrawTextG(g, L"Processing setup", 0.f, (float)TOOLBAR_H + 50.f, (float)rc.right, 40.f, 24.f, C_TEXT, true, 0);

            RectI outPick{ PAD, TOOLBAR_H + 90, 420, 38 };
            RectI startBtn{ PAD, TOOLBAR_H + 150, 160, 36 };

            SolidBrush box(Color(255, 28, 28, 28));
            Pen br(C_BORDER, 1.0f);
            g.FillRectangle(&box, outPick.x, outPick.y, outPick.w, outPick.h);
            g.DrawRectangle(&br, outPick.x, outPick.y, outPick.w, outPick.h);

            Pen acc2(C_ACC, 2.0f);
            g.DrawLine(&acc2, outPick.x + 2, outPick.y + outPick.h - 1, outPick.x + outPick.w - 2, outPick.y + outPick.h - 1);

            std::wstring outLine = g_outputFolder.empty() ? L"Choose output folder..." : EllipsizePath(g_outputFolder, 80);
            DrawTextG(g, outLine, (float)outPick.x + 10.f, (float)outPick.y, (float)outPick.w - 20.f, (float)outPick.h, 13.f, g_outputFolder.empty() ? C_SUB : C_TEXT, false, -1);

            bool canStart = !g_outputFolder.empty() && !g_images.empty() && !g_processing;
            DrawButton(g, startBtn, g_processing ? L"Processing" : L"Start", !canStart);

            int barX = PAD;
            int barY = TOOLBAR_H + 230;
            int barW = rc.right - PAD * 2;
            int barH = 18;

            SolidBrush barBg(Color(255, 40, 40, 40));
            g.FillRectangle(&barBg, barX, barY, barW, barH);

            int total = max(1, (int)g_images.size());
            int done = ClampI(g_processed, 0, total);
            float k = (float)done / (float)total;
            int fillW = (int)(barW * k);

            SolidBrush barFill(C_ACC);
            g.FillRectangle(&barFill, barX, barY, fillW, barH);

            Pen barBr(C_BORDER, 1.0f);
            g.DrawRectangle(&barBr, barX, barY, barW, barH);

            std::wstring ptxt = L"Processed: " + std::to_wstring(done) + L" / " + std::to_wstring((int)g_images.size());
            DrawTextG(g, ptxt, (float)PAD, (float)barY + 26.f, (float)(rc.right - PAD * 2), 26.f, 14.f, C_SUB, false, -1);
        }
        else
        {
            DrawTextG(g, L"All done!", 0.f, (float)TOOLBAR_H + 40.f, (float)rc.right, 44.f, 28.f, C_TEXT, true, 0);
            DrawTextG(g, L"Click Back to return to gallery", 0.f, (float)TOOLBAR_H + 80.f, (float)rc.right, 30.f, 14.f, C_SUB, false, 0);

            int cx = rc.right / 2;
            int cy = TOOLBAR_H + 330;
            DrawCat(g, cx, cy, g_doneTick);
        }

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_SCROLL);
        KillTimer(hWnd, TIMER_PROCESS);
        KillTimer(hWnd, TIMER_DONE_ANIM);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

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
