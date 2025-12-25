#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <gdiplus.h>
#include <algorithm>
#include <memory>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "Shell32.lib")

using namespace Gdiplus;

// ================== Layout ==================
constexpr int TOOLBAR_H = 62;
constexpr int FOOTER_H = 42;

constexpr int GRID_PADDING = 16;
constexpr int CELL_SIZE = 140;
constexpr int CELL_GAP = 16;

constexpr int BTN_W = 140;
constexpr int BTN_H = 34;

constexpr int NAV_BTN_W = 160;
constexpr int NAV_BTN_H = 34;

// ================== App ==================
constexpr wchar_t WINDOW_CLASS[] = L"MangaDenoiserWindow";
constexpr wchar_t WINDOW_TITLE[] = L"Manga Denoiser";

constexpr UINT_PTR TIMER_SCROLL = 1;
constexpr UINT_PTR TIMER_PROCESS = 2;
constexpr UINT_PTR TIMER_ANIM = 3;

// ================== Theme ==================
static const Color C_BG(255, 20, 20, 20);
static const Color C_PANEL(255, 30, 30, 30);
static const Color C_TEXT(255, 220, 220, 220);
static const Color C_MUTED(255, 160, 160, 160);

// accent (pink) – minimal but present
static const Color C_ACCENT(255, 255, 105, 180); // hot pink
static const Color C_ACCENT_SOFT(120, 255, 105, 180);

// ================== State ==================
enum class AppScreen { Gallery, Processing, Done };

ULONG_PTR g_gdiplusToken = 0;

AppScreen g_screen = AppScreen::Gallery;

std::vector<std::wstring> g_images;     // full paths
std::vector<std::unique_ptr<Bitmap>> g_thumbs; // lazy cache: nullptr until built
std::wstring g_inputFolder;
std::wstring g_outputFolder;

std::wstring g_status = L"Ready";

int g_selected = -1;

// smooth scroll
int g_scrollY = 0;
int g_scrollTarget = 0;
int g_scrollMax = 0;
bool g_scrollTimerOn = false;

// processing (fake)
bool g_processing = false;
int  g_processed = 0;

// done animation
int  g_animTick = 0;
bool g_animOn = false;

// double buffer
HDC g_memDC = nullptr;
HBITMAP g_memBmp = nullptr;
HBITMAP g_memOld = nullptr;
int g_memW = 0, g_memH = 0;

// ================== Helpers ==================
static void EnsureBackBuffer(HDC refDC, int w, int h)
{
    if (w <= 0 || h <= 0) return;

    if (g_memDC && (w == g_memW && h == g_memH))
        return;

    if (!g_memDC)
    {
        g_memDC = CreateCompatibleDC(refDC);
    }

    if (g_memBmp)
    {
        SelectObject(g_memDC, g_memOld);
        DeleteObject(g_memBmp);
        g_memBmp = nullptr;
        g_memOld = nullptr;
    }

    g_memBmp = CreateCompatibleBitmap(refDC, w, h);
    g_memOld = (HBITMAP)SelectObject(g_memDC, g_memBmp);
    g_memW = w; g_memH = h;
}

static void ReleaseBackBuffer()
{
    if (g_memDC)
    {
        if (g_memBmp)
        {
            SelectObject(g_memDC, g_memOld);
            DeleteObject(g_memBmp);
            g_memBmp = nullptr;
            g_memOld = nullptr;
        }
        DeleteDC(g_memDC);
        g_memDC = nullptr;
    }
    g_memW = g_memH = 0;
}

static bool IsImageFile(const std::wstring& p)
{
    auto dot = p.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = p.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext == L"png" || ext == L"jpg" || ext == L"jpeg" || ext == L"bmp" || ext == L"webp";
}

static void ClearImages()
{
    g_images.clear();
    g_thumbs.clear();
    g_selected = -1;
    g_scrollY = g_scrollTarget = 0;
    g_scrollMax = 0;
}

static void AddImagesFromFolder(const std::wstring& folder)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\*.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring f = folder + L"\\" + fd.cFileName;
        if (IsImageFile(f))
            g_images.push_back(f);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

static void EnsureThumbsSize()
{
    g_thumbs.clear();
    g_thumbs.resize(g_images.size());
}

static std::unique_ptr<Bitmap> MakeThumb(const std::wstring& path)
{
    // lazy thumb build – keeps scroll smooth after first pass
    std::unique_ptr<Image> img(new Image(path.c_str()));
    if (img->GetLastStatus() != Ok) return nullptr;

    const int S = CELL_SIZE;
    std::unique_ptr<Bitmap> thumb(new Bitmap(S, S, PixelFormat32bppARGB));

    Graphics g(thumb.get());
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetSmoothingMode(SmoothingModeHighQuality);

    // background
    SolidBrush bg(Color(255, 26, 26, 26));
    g.FillRectangle(&bg, 0, 0, S, S);

    const int iw = (int)img->GetWidth();
    const int ih = (int)img->GetHeight();
    if (iw <= 0 || ih <= 0) return nullptr;

    float scale = min((float)(S - 8) / iw, (float)(S - 8) / ih);
    int w = (int)(iw * scale);
    int h = (int)(ih * scale);
    int x = (S - w) / 2;
    int y = (S - h) / 2;

    g.DrawImage(img.get(), x, y, w, h);

    // subtle border
    Pen br(Color(255, 60, 60, 60), 1.f);
    g.DrawRectangle(&br, 0, 0, S - 1, S - 1);

    return thumb;
}

static std::wstring GetDesktopPath()
{
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)))
    {
        out = p;
        CoTaskMemFree(p);
    }
    return out;
}

static bool PickFolderDialog(HWND hWnd, const wchar_t* title, std::wstring& outFolder)
{
    BROWSEINFOW bi{};
    wchar_t path[MAX_PATH]{};

    bi.hwndOwner = hWnd;
    bi.pszDisplayName = path;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;

    bool ok = SHGetPathFromIDListW(pidl, path) != FALSE;
    CoTaskMemFree(pidl);
    if (!ok) return false;

    outFolder = path;
    return true;
}

// ================== Layout math ==================
static int ComputeCols(int clientW)
{
    int usable = clientW - GRID_PADDING * 2;
    int step = CELL_SIZE + CELL_GAP;
    return max(1, usable / step);
}

static int ComputeContentH(int cols)
{
    if (g_images.empty()) return GRID_PADDING * 2;
    int rows = (int)((g_images.size() + cols - 1) / cols);
    return GRID_PADDING * 2 + rows * (CELL_SIZE + CELL_GAP);
}

static int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void StartScrollTimer(HWND hWnd)
{
    if (!g_scrollTimerOn)
    {
        g_scrollTimerOn = true;
        SetTimer(hWnd, TIMER_SCROLL, 16, nullptr);
    }
}

static void StopScrollTimer(HWND hWnd)
{
    if (g_scrollTimerOn)
    {
        g_scrollTimerOn = false;
        KillTimer(hWnd, TIMER_SCROLL);
    }
}

static void StartAnimTimer(HWND hWnd)
{
    if (!g_animOn)
    {
        g_animOn = true;
        SetTimer(hWnd, TIMER_ANIM, 33, nullptr);
    }
}

static void StopAnimTimer(HWND hWnd)
{
    if (g_animOn)
    {
        g_animOn = false;
        KillTimer(hWnd, TIMER_ANIM);
    }
}

// ================== Hit testing ==================
static bool HitRect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh;
}

static int HitGridIndex(const RECT& rc, int mx, int my)
{
    int top = TOOLBAR_H;
    int bottom = rc.bottom - FOOTER_H;
    if (my < top || my > bottom) return -1;

    int cols = ComputeCols(rc.right);
    int localX = mx - GRID_PADDING;
    int localY = (my - top) + g_scrollY - GRID_PADDING;
    if (localX < 0 || localY < 0) return -1;

    int step = CELL_SIZE + CELL_GAP;
    int col = localX / step;
    int row = localY / step;

    if (col < 0 || col >= cols) return -1;

    int xIn = localX % step;
    int yIn = localY % step;
    if (xIn > CELL_SIZE || yIn > CELL_SIZE) return -1;

    int idx = row * cols + col;
    if (idx < 0 || idx >= (int)g_images.size()) return -1;
    return idx;
}

// ================== Drawing ==================
static void DrawTextEllipsis(Graphics& g, const wchar_t* text, const RectF& r, const Color& c, float size, bool bold = false)
{
    FontFamily ff(L"Segoe UI");
    Font f(&ff, size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    SolidBrush br(c);

    StringFormat sf;
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    sf.SetLineAlignment(StringAlignmentCenter);

    g.DrawString(text, -1, &f, r, &sf, &br);
}

static void DrawButton(Graphics& g, int x, int y, int w, int h, const wchar_t* text, bool disabled, bool accentBorder)
{
    Color bg = disabled ? Color(255, 38, 38, 38) : Color(255, 50, 50, 50);
    Color br = accentBorder ? C_ACCENT : Color(255, 85, 85, 85);
    Color tx = disabled ? Color(255, 120, 120, 120) : C_TEXT;

    SolidBrush b(bg);
    Pen p(br, accentBorder ? 2.f : 1.f);

    g.FillRectangle(&b, x, y, w, h);
    g.DrawRectangle(&p, x, y, w, h);

    FontFamily ff(L"Segoe UI");
    Font f(&ff, 14.f, FontStyleRegular, UnitPixel);
    SolidBrush tb(tx);

    RectF r((REAL)x, (REAL)y, (REAL)w, (REAL)h);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(text, -1, &f, r, &sf, &tb);
}

static void DrawTopAccentLine(Graphics& g, int w)
{
    Pen p(C_ACCENT_SOFT, 2.f);
    g.DrawLine(&p, 0.f, (REAL)TOOLBAR_H - 1.f, (REAL)w, (REAL)TOOLBAR_H - 1.f);
}

static void DrawCenteredHint(Graphics& g, const RECT& rc, const wchar_t* text)
{
    RectF r(0.f, (REAL)TOOLBAR_H, (REAL)rc.right, (REAL)(rc.bottom - TOOLBAR_H - FOOTER_H));
    FontFamily ff(L"Segoe UI");
    Font f(&ff, 20.f, FontStyleBold, UnitPixel);
    SolidBrush br(C_TEXT);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(text, -1, &f, r, &sf, &br);
}

static void DrawProgressBar(Graphics& g, int x, int y, int w, int h, float t01)
{
    t01 = max(0.f, min(1.f, t01));

    SolidBrush bg(Color(255, 40, 40, 40));
    g.FillRectangle(&bg, x, y, w, h);

    int fill = (int)(w * t01);
    SolidBrush fillBr(C_ACCENT);
    g.FillRectangle(&fillBr, x, y, fill, h);

    Pen br(Color(255, 80, 80, 80), 1.f);
    g.DrawRectangle(&br, x, y, w, h);
}

static void DrawCuteCat(Graphics& g, int cx, int cy, int tick)
{
    // Very simple vector cat with subtle animation (tail + blush pulse)
    g.SetSmoothingMode(SmoothingModeHighQuality);

    float pulse = 0.5f + 0.5f * sinf(tick * 0.08f);
    int blushA = (int)(80 + 80 * pulse);

    // body
    SolidBrush body(Color(255, 50, 50, 55));
    g.FillEllipse(&body, cx - 90, cy - 60, 180, 130);

    // ears
    SolidBrush ear(Color(255, 55, 55, 60));
    Point ear1[3] = { Point(cx - 65, cy - 45), Point(cx - 95, cy - 105), Point(cx - 30, cy - 70) };
    Point ear2[3] = { Point(cx + 65, cy - 45), Point(cx + 95, cy - 105), Point(cx + 30, cy - 70) };
    g.FillPolygon(&ear, ear1, 3);
    g.FillPolygon(&ear, ear2, 3);

    // accent outline
    Pen outline(C_ACCENT_SOFT, 3.f);
    g.DrawEllipse(&outline, cx - 92, cy - 62, 184, 134);

    // face (eyes)
    Pen eye(Color(255, 230, 230, 230), 4.f);
    g.DrawLine(&eye, cx - 40, cy - 10, cx - 20, cy - 10);
    g.DrawLine(&eye, cx + 20, cy - 10, cx + 40, cy - 10);

    // mouth
    Pen mouth(Color(255, 230, 230, 230), 3.f);
    g.DrawArc(&mouth, cx - 12, cy + 2, 24, 18, 20, 140);

    // blush
    SolidBrush blush(Color(blushA, 255, 105, 180));
    g.FillEllipse(&blush, cx - 62, cy + 2, 22, 12);
    g.FillEllipse(&blush, cx + 40, cy + 2, 22, 12);

    // tail animation (swing)
    float a = sinf(tick * 0.07f) * 18.f;
    Pen tail(Color(200, 255, 105, 180), 10.f);
    tail.SetStartCap(LineCapRound);
    tail.SetEndCap(LineCapRound);
    g.DrawArc(&tail, cx + 65, cy + 10, 120, 90, 220 + a, 80);

    // little heart
    SolidBrush heart(C_ACCENT);
    g.FillEllipse(&heart, cx + 90, cy - 95, 12, 12);
    g.FillEllipse(&heart, cx + 102, cy - 95, 12, 12);
    Point tri[3] = { Point(cx + 90, cy - 89), Point(cx + 114, cy - 89), Point(cx + 102, cy - 74) };
    g.FillPolygon(&heart, tri, 3);
}

// ================== Screen transitions ==================
static void GoToGallery(HWND hWnd)
{
    g_screen = AppScreen::Gallery;
    g_processing = false;
    g_processed = 0;
    KillTimer(hWnd, TIMER_PROCESS);
    StopAnimTimer(hWnd);

    g_status = L"Ready";
    InvalidateRect(hWnd, nullptr, TRUE);
}

static void GoToProcessing(HWND hWnd)
{
    if (g_images.empty()) { g_status = L"No images to process"; InvalidateRect(hWnd, nullptr, TRUE); return; }

    g_screen = AppScreen::Processing;
    StopAnimTimer(hWnd);

    // default output folder if empty
    if (g_outputFolder.empty())
    {
        std::wstring desk = GetDesktopPath();
        g_outputFolder = desk.empty() ? L"" : (desk + L"\\MangaDenoiser_Output");
    }

    g_processing = true;
    g_processed = 0;
    g_status = L"Processing (stub)";

    SetTimer(hWnd, TIMER_PROCESS, 25, nullptr);
    InvalidateRect(hWnd, nullptr, TRUE);
}

static void GoToDone(HWND hWnd)
{
    g_screen = AppScreen::Done;
    g_processing = false;
    g_status = L"Done";
    StartAnimTimer(hWnd);
    InvalidateRect(hWnd, nullptr, TRUE);
}

// ================== Main WndProc ==================
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        DragAcceptFiles(hWnd, TRUE);
        return 0;

    case WM_ERASEBKGND:
        // prevent flicker (we draw entire frame ourselves)
        return 1;

    case WM_SIZE:
        // backbuffer will be resized on next paint
        InvalidateRect(hWnd, nullptr, TRUE);
        return 0;

    case WM_DROPFILES:
    {
        HDROP hDrop = (HDROP)wParam;
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        wchar_t path[MAX_PATH];

        ClearImages();
        g_inputFolder.clear();

        for (UINT i = 0; i < count; i++)
        {
            if (!DragQueryFileW(hDrop, i, path, MAX_PATH)) continue;
            std::wstring p = path;

            DWORD attr = GetFileAttributesW(p.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES) continue;

            if (attr & FILE_ATTRIBUTE_DIRECTORY)
            {
                // take first folder as "input folder" display, but still add all folders
                if (g_inputFolder.empty()) g_inputFolder = p;
                AddImagesFromFolder(p);
            }
            else if (IsImageFile(p))
            {
                g_images.push_back(p);
            }
        }

        EnsureThumbsSize();

        if (!g_images.empty())
        {
            g_selected = 0;
            g_status = L"Images loaded";
        }
        else
        {
            g_status = L"No images found";
        }

        DragFinish(hDrop);
        InvalidateRect(hWnd, nullptr, TRUE);
        return 0;
    }

    case WM_MOUSEWHEEL:
        if (g_screen == AppScreen::Gallery && !g_images.empty())
        {
            short d = GET_WHEEL_DELTA_WPARAM(wParam);
            g_scrollTarget -= (int)(d * 0.75); // smoother, smaller steps
            g_scrollTarget = Clamp(g_scrollTarget, 0, g_scrollMax);
            StartScrollTimer(hWnd);
        }
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_SCROLL)
        {
            int diff = g_scrollTarget - g_scrollY;
            if (abs(diff) <= 1)
            {
                g_scrollY = g_scrollTarget;
                StopScrollTimer(hWnd);
            }
            else
            {
                g_scrollY += diff / 5; // easing
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == TIMER_PROCESS)
        {
            // fake processing: just count images
            if (!g_processing)
            {
                KillTimer(hWnd, TIMER_PROCESS);
                return 0;
            }

            g_processed++;
            if (g_processed >= (int)g_images.size())
            {
                KillTimer(hWnd, TIMER_PROCESS);
                g_processing = false;
                GoToDone(hWnd);
                return 0;
            }

            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == TIMER_ANIM)
        {
            g_animTick++;
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        return 0;

    case WM_LBUTTONDOWN:
    {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);

        RECT rc; GetClientRect(hWnd, &rc);

        // ----- Toolbar hitboxes -----
        int btnY = (TOOLBAR_H - BTN_H) / 2;

        // Buttons: Add Folder, Change Output (on processing), Next/Back on footer
        if (y < TOOLBAR_H)
        {
            // Add folder
            if (HitRect(x, y, 16, btnY, BTN_W, BTN_H))
            {
                std::wstring picked;
                if (PickFolderDialog(hWnd, L"Select folder with images", picked))
                {
                    ClearImages();
                    g_inputFolder = picked;
                    AddImagesFromFolder(g_inputFolder);
                    EnsureThumbsSize();

                    if (!g_images.empty())
                    {
                        g_selected = 0;
                        g_status = L"Images loaded";
                    }
                    else
                    {
                        g_status = L"No images found";
                    }

                    // reset scroll properly (and clamp)
                    g_scrollY = g_scrollTarget = 0;
                    InvalidateRect(hWnd, nullptr, TRUE);
                }
                return 0;
            }

            // On Processing screen: Change output folder
            if (g_screen == AppScreen::Processing)
            {
                int changeX = 16 + BTN_W + 12;
                if (HitRect(x, y, changeX, btnY, BTN_W, BTN_H))
                {
                    std::wstring picked;
                    if (PickFolderDialog(hWnd, L"Select output folder", picked))
                    {
                        g_outputFolder = picked;
                        g_status = L"Output folder set";
                        InvalidateRect(hWnd, nullptr, TRUE);
                    }
                    return 0;
                }
            }

            return 0;
        }

        // ----- Footer navigation -----
        int footerTop = rc.bottom - FOOTER_H;
        if (y >= footerTop)
        {
            int navY = footerTop + (FOOTER_H - NAV_BTN_H) / 2;

            bool canBack = true;
            bool canNext = true;

            if (g_screen == AppScreen::Gallery)
            {
                canBack = false;
                canNext = !g_images.empty();
            }
            else if (g_screen == AppScreen::Processing)
            {
                // During processing: allow Cancel only via Back? We'll make Back return to Gallery only if not processing.
                canBack = !g_processing;
                canNext = !g_processing && (g_processed >= (int)g_images.size());
            }
            else if (g_screen == AppScreen::Done)
            {
                canBack = true; // back to gallery
                canNext = false;
            }

            // Back button (left)
            if (HitRect(x, y, 16, navY, NAV_BTN_W, NAV_BTN_H))
            {
                if (g_screen == AppScreen::Done)
                {
                    GoToGallery(hWnd);
                }
                else if (g_screen == AppScreen::Processing && canBack)
                {
                    GoToGallery(hWnd);
                }
                return 0;
            }

            // Next button (right)
            int nextX = rc.right - 16 - NAV_BTN_W;
            if (HitRect(x, y, nextX, navY, NAV_BTN_W, NAV_BTN_H))
            {
                if (g_screen == AppScreen::Gallery && canNext)
                {
                    GoToProcessing(hWnd);
                }
                else if (g_screen == AppScreen::Processing && canNext)
                {
                    GoToDone(hWnd);
                }
                return 0;
            }

            return 0;
        }

        // ----- Grid selection -----
        if (g_screen == AppScreen::Gallery && !g_images.empty())
        {
            int idx = HitGridIndex(rc, x, y);
            if (idx != -1)
            {
                g_selected = idx;
                g_status = L"Selected: " + std::to_wstring(idx + 1);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rc;
        GetClientRect(hWnd, &rc);

        EnsureBackBuffer(hdc, rc.right, rc.bottom);

        // Clear backbuffer
        {
            Graphics gg(g_memDC);
            SolidBrush bg(C_BG);
            gg.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);
        }

        Graphics g(g_memDC);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetSmoothingMode(SmoothingModeHighQuality);

        // ===== Toolbar =====
        SolidBrush tb(C_PANEL);
        g.FillRectangle(&tb, 0, 0, rc.right, TOOLBAR_H);
        DrawTopAccentLine(g, rc.right);

        int btnY = (TOOLBAR_H - BTN_H) / 2;

        // Add folder button (accent border minimal)
        DrawButton(g, 16, btnY, BTN_W, BTN_H, L"Add folder", false, true);

        // Processing screen: Change output button
        if (g_screen == AppScreen::Processing)
        {
            DrawButton(g, 16 + BTN_W + 12, btnY, BTN_W, BTN_H, L"Output...", false, false);
        }

        // Show input folder path (Gallery + Processing), ellipsis
        {
            std::wstring shown = g_inputFolder.empty() ? L"Input: (none)" : (L"Input: " + g_inputFolder);
            RectF r((REAL)(16 + BTN_W + 12 + (g_screen == AppScreen::Processing ? (BTN_W + 12) : 0)),
                0.f,
                (REAL)(rc.right - 24),
                (REAL)TOOLBAR_H);

            DrawTextEllipsis(g, shown.c_str(), r, C_MUTED, 13.f, false);
        }

        // ===== Footer (ALWAYS visible) =====
        SolidBrush fb(C_PANEL);
        g.FillRectangle(&fb, 0, rc.bottom - FOOTER_H, rc.right, FOOTER_H);

        // Accent line above footer
        Pen fp(C_ACCENT_SOFT, 2.f);
        g.DrawLine(&fp, 0.f, (REAL)(rc.bottom - FOOTER_H), (REAL)rc.right, (REAL)(rc.bottom - FOOTER_H));

        // Footer text
        {
            FontFamily ff(L"Segoe UI");
            Font font(&ff, 13.f, FontStyleRegular, UnitPixel);
            SolidBrush tx(C_TEXT);

            std::wstring left =
                L"Images: " + std::to_wstring(g_images.size()) +
                L"    Selected: " + std::to_wstring(g_selected >= 0 ? (g_selected + 1) : 0) +
                L"    Status: " + g_status;

            RectF fr(16.f, (REAL)(rc.bottom - FOOTER_H + 10), (REAL)(rc.right - 32), 18.f);
            g.DrawString(left.c_str(), -1, &font, fr, nullptr, &tx);
        }

        // Footer navigation buttons (installer-like)
        {
            int navY = rc.bottom - FOOTER_H + (FOOTER_H - NAV_BTN_H) / 2;

            bool canBack = true, canNext = true;
            const wchar_t* backText = L"Back";
            const wchar_t* nextText = L"Next";

            if (g_screen == AppScreen::Gallery)
            {
                canBack = false;
                canNext = !g_images.empty();
            }
            else if (g_screen == AppScreen::Processing)
            {
                // while processing, next disabled; back disabled
                canBack = !g_processing;
                canNext = !g_processing && (g_processed >= (int)g_images.size());
                backText = g_processing ? L"Back" : L"Back";
                nextText = g_processing ? L"Processing..." : L"Next";
            }
            else if (g_screen == AppScreen::Done)
            {
                canBack = true;
                canNext = false;
                backText = L"Back to Gallery";
            }

            // Back (left)
            DrawButton(g, 16, navY, NAV_BTN_W, NAV_BTN_H, backText, !canBack, false);

            // Next (right)
            int nextX = rc.right - 16 - NAV_BTN_W;
            DrawButton(g, nextX, navY, NAV_BTN_W, NAV_BTN_H, nextText, !canNext, true);
        }

        // ===== Screen content area =====
        int contentTop = TOOLBAR_H;
        int contentBottom = rc.bottom - FOOTER_H;
        int contentH = contentBottom - contentTop;

        // --- Gallery ---
        if (g_screen == AppScreen::Gallery)
        {
            // compute scroll bounds
            int cols = ComputeCols(rc.right);
            int contentTotalH = ComputeContentH(cols);
            g_scrollMax = max(0, contentTotalH - contentH);
            g_scrollTarget = Clamp(g_scrollTarget, 0, g_scrollMax);
            g_scrollY = Clamp(g_scrollY, 0, g_scrollMax);

            if (g_images.empty())
            {
                DrawCenteredHint(g, rc, L"Drop images/folder here or click 'Add folder'");
            }
            else
            {
                int yOffset = contentTop + GRID_PADDING - g_scrollY;

                for (size_t i = 0; i < g_images.size(); i++)
                {
                    int col = (int)(i % cols);
                    int row = (int)(i / cols);

                    int x = GRID_PADDING + col * (CELL_SIZE + CELL_GAP);
                    int y = yOffset + row * (CELL_SIZE + CELL_GAP);

                    if (y + CELL_SIZE < contentTop || y > contentBottom) continue;

                    Rect r(x, y, CELL_SIZE, CELL_SIZE);

                    // lazy thumb build
                    if (!g_thumbs[i])
                        g_thumbs[i] = MakeThumb(g_images[i]);

                    if (g_thumbs[i])
                        g.DrawImage(g_thumbs[i].get(), r);
                    else
                    {
                        SolidBrush bad(Color(255, 45, 45, 45));
                        g.FillRectangle(&bad, r);
                    }

                    // selection (pink minimal)
                    if ((int)i == g_selected)
                    {
                        Pen p(C_ACCENT, 3.f);
                        g.DrawRectangle(&p, r);
                        Pen p2(C_ACCENT_SOFT, 1.f);
                        Rect inner(r.X + 3, r.Y + 3, r.Width - 6, r.Height - 6);
                        g.DrawRectangle(&p2, inner);
                    }
                }

                // subtle scroll indicator (mini)
                if (g_scrollMax > 0)
                {
                    float t = (float)g_scrollY / (float)g_scrollMax;
                    int barH = max(40, (int)(contentH * 0.18));
                    int barY = contentTop + (int)((contentH - barH) * t);
                    int barX = rc.right - 8;
                    Pen sp(C_ACCENT_SOFT, 3.f);
                    g.DrawLine(&sp, (REAL)barX, (REAL)barY, (REAL)barX, (REAL)(barY + barH));
                }
            }
        }
        // --- Processing ---
        else if (g_screen == AppScreen::Processing)
        {
            SolidBrush panel(Color(255, 24, 24, 24));
            g.FillRectangle(&panel, 0, contentTop, rc.right, contentH);

            // Title
            {
                FontFamily ff(L"Segoe UI");
                Font title(&ff, 26.f, FontStyleBold, UnitPixel);
                SolidBrush tx(C_TEXT);

                RectF tr(24.f, (REAL)contentTop + 28.f, (REAL)(rc.right - 48), 40.f);
                g.DrawString(L"Processing", -1, &title, tr, nullptr, &tx);
            }

            // Output folder line (ellipsized)
            {
                std::wstring line = g_outputFolder.empty() ? L"Output: (not set)" : (L"Output: " + g_outputFolder);
                RectF r(24.f, (REAL)contentTop + 78.f, (REAL)(rc.right - 48), 22.f);
                DrawTextEllipsis(g, line.c_str(), r, C_MUTED, 13.f, false);
            }

            // Progress text
            {
                FontFamily ff(L"Segoe UI");
                Font f(&ff, 16.f, FontStyleRegular, UnitPixel);
                SolidBrush tx(C_TEXT);

                std::wstring p =
                    L"Images processed: " + std::to_wstring(g_processed) +
                    L" / " + std::to_wstring((int)g_images.size());

                RectF r(24.f, (REAL)contentTop + 120.f, (REAL)(rc.right - 48), 26.f);
                g.DrawString(p.c_str(), -1, &f, r, nullptr, &tx);
            }

            // Progress bar
            float t01 = g_images.empty() ? 0.f : (float)g_processed / (float)g_images.size();
            DrawProgressBar(g, 24, contentTop + 160, rc.right - 48, 16, t01);

            // A hint
            {
                RectF r(24.f, (REAL)contentTop + 196.f, (REAL)(rc.right - 48), 22.f);
                DrawTextEllipsis(g,
                    g_processing ? L"Working... (denoise will be added later)" : L"Ready. Click Next to continue.",
                    r, C_MUTED, 13.f, false);
            }

            // minimal accent frame around content
            Pen frame(C_ACCENT_SOFT, 2.f);
            g.DrawRectangle(&frame, 16, contentTop + 16, rc.right - 32, contentH - 32);
        }
        // --- Done ---
        else
        {
            SolidBrush panel(Color(255, 24, 24, 24));
            g.FillRectangle(&panel, 0, contentTop, rc.right, contentH);

            // Title
            {
                FontFamily ff(L"Segoe UI");
                Font title(&ff, 26.f, FontStyleBold, UnitPixel);
                SolidBrush tx(C_TEXT);

                RectF tr(24.f, (REAL)contentTop + 28.f, (REAL)(rc.right - 48), 40.f);
                g.DrawString(L"All done!", -1, &title, tr, nullptr, &tx);
            }

            // Cat animation
            int cx = rc.right / 2;
            int cy = contentTop + contentH / 2 + 30;
            DrawCuteCat(g, cx, cy, g_animTick);

            // Subtitle
            {
                RectF r(24.f, (REAL)(contentTop + 88), (REAL)(rc.right - 48), 22.f);
                DrawTextEllipsis(g, L"You can go back to Gallery and process another folder.", r, C_MUTED, 13.f, false);
            }

            // accent frame
            Pen frame(C_ACCENT_SOFT, 2.f);
            g.DrawRectangle(&frame, 16, contentTop + 16, rc.right - 32, contentH - 32);
        }

        // ===== present backbuffer =====
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, g_memDC, 0, 0, SRCCOPY);

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        StopScrollTimer(hWnd);
        StopAnimTimer(hWnd);
        KillTimer(hWnd, TIMER_PROCESS);
        ReleaseBackBuffer();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ================== WinMain ==================
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    GdiplusStartupInput gd;
    GdiplusStartup(&g_gdiplusToken, &gd, nullptr);

    // init default output folder
    std::wstring desk = GetDesktopPath();
    if (!desk.empty()) g_outputFolder = desk + L"\\MangaDenoiser_Output";

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = WINDOW_CLASS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // we fully paint ourselves
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
    return 0;
}
