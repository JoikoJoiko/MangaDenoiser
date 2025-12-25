#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

// ---------- Layout ----------
constexpr int TOOLBAR_HEIGHT = 56;
constexpr int FOOTER_HEIGHT = 32;
constexpr int GRID_PADDING = 16;
constexpr int CELL_SIZE = 140;
constexpr int CELL_GAP = 16;

// ---------- App ----------
constexpr wchar_t WINDOW_CLASS[] = L"MangaDenoiserWindow";
constexpr wchar_t WINDOW_TITLE[] = L"Manga Denoiser";

ULONG_PTR g_gdiplusToken;

std::vector<std::wstring> g_images;
int g_scrollY = 0;
int g_scrollMax = 0;

std::wstring g_status = L"Ready";

// ---------- Utils ----------
bool IsImageFile(const std::wstring& p)
{
    auto dot = p.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;

    std::wstring ext = p.substr(dot + 1);
    for (auto& c : ext) c = towlower(c);

    return ext == L"png" || ext == L"jpg" || ext == L"jpeg" || ext == L"bmp" || ext == L"webp";
}

void AddImagesFromFolder(const std::wstring& folder)
{
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW((folder + L"\\*.*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            std::wstring f = folder + L"\\" + fd.cFileName;
            if (IsImageFile(f))
                g_images.push_back(f);
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

void AddImagesFromDrop(HDROP hDrop)
{
    g_images.clear();
    g_scrollY = 0;

    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    wchar_t path[MAX_PATH];

    for (UINT i = 0; i < count; i++)
    {
        DragQueryFileW(hDrop, i, path, MAX_PATH);
        std::wstring p = path;

        DWORD attr = GetFileAttributesW(p.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;

        if (attr & FILE_ATTRIBUTE_DIRECTORY)
            AddImagesFromFolder(p);
        else if (IsImageFile(p))
            g_images.push_back(p);
    }
}

// ---------- Scroll ----------
void UpdateScroll(HWND hWnd, int clientHeight, int contentHeight)
{
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = contentHeight;
    si.nPage = clientHeight;
    si.nPos = g_scrollY;

    SetScrollInfo(hWnd, SB_VERT, &si, TRUE);
    g_scrollMax = max(0, contentHeight - clientHeight);
}

// ---------- Drawing ----------
void DrawButton(Graphics& g, int x, int y, int w, int h, const wchar_t* text)
{
    SolidBrush bg(Color(255, 45, 45, 45));
    Pen border(Color(255, 90, 90, 90));

    g.FillRectangle(&bg, x, y, w, h);
    g.DrawRectangle(&border, x, y, w, h);

    FontFamily ff(L"Segoe UI");
    Font font(&ff, 14, FontStyleRegular, UnitPixel);
    SolidBrush textBrush(Color(255, 220, 220, 220));

    RectF r((REAL)x, (REAL)y, (REAL)w, (REAL)h);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);

    g.DrawString(text, -1, &font, r, &sf, &textBrush);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        DragAcceptFiles(hWnd, TRUE);
        SetClassLongPtr(hWnd, GCLP_HBRBACKGROUND,
            (LONG_PTR)CreateSolidBrush(RGB(24, 24, 24)));
        return 0;

    case WM_DROPFILES:
        AddImagesFromDrop((HDROP)wParam);
        DragFinish((HDROP)wParam);
        g_status = L"Images loaded";
        InvalidateRect(hWnd, nullptr, TRUE);
        return 0;

    case WM_VSCROLL:
    {
        int action = LOWORD(wParam);
        int delta = 0;

        if (action == SB_LINEUP) delta = -40;
        if (action == SB_LINEDOWN) delta = 40;
        if (action == SB_THUMBTRACK)
        {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_TRACKPOS;
            GetScrollInfo(hWnd, SB_VERT, &si);
            g_scrollY = si.nTrackPos;
            InvalidateRect(hWnd, nullptr, TRUE);
            return 0;
        }

        g_scrollY = max(0, min(g_scrollY + delta, g_scrollMax));
        InvalidateRect(hWnd, nullptr, TRUE);
        return 0;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        Graphics g(hdc);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

        // Toolbar
        SolidBrush tb(Color(255, 30, 30, 30));
        g.FillRectangle(&tb, 0, 0, rc.right, TOOLBAR_HEIGHT);

        DrawButton(g, 16, 12, 120, 32, L"Add folder");
        DrawButton(g, 152, 12, 120, 32, L"Process");
        DrawButton(g, 288, 12, 120, 32, L"Save");

        // Footer
        SolidBrush fb(Color(255, 30, 30, 30));
        g.FillRectangle(&fb, 0, rc.bottom - FOOTER_HEIGHT, rc.right, FOOTER_HEIGHT);

        FontFamily ff(L"Segoe UI");
        Font font(&ff, 13, FontStyleRegular, UnitPixel);
        SolidBrush txt(Color(255, 180, 180, 180));

        RectF footerText(16, (REAL)(rc.bottom - FOOTER_HEIGHT + 8), 500, 20);
        std::wstring footer = L"Images: " + std::to_wstring(g_images.size()) + L"    Status: " + g_status;
        g.DrawString(footer.c_str(), -1, &font, footerText, nullptr, &txt);

        // Grid
        int gridTop = TOOLBAR_HEIGHT;
        int gridBottom = rc.bottom - FOOTER_HEIGHT;
        int gridHeight = gridBottom - gridTop;

        int cols = max(1, (rc.right - GRID_PADDING * 2) / (CELL_SIZE + CELL_GAP));
        int rows = (int)((g_images.size() + cols - 1) / cols);
        int contentHeight = rows * (CELL_SIZE + CELL_GAP) + GRID_PADDING * 2;

        UpdateScroll(hWnd, gridHeight, contentHeight);

        int yOffset = gridTop + GRID_PADDING - g_scrollY;

        for (size_t i = 0; i < g_images.size(); i++)
        {
            int col = (int)(i % cols);
            int row = (int)(i / cols);

            int x = GRID_PADDING + col * (CELL_SIZE + CELL_GAP);
            int y = yOffset + row * (CELL_SIZE + CELL_GAP);

            if (y + CELL_SIZE < gridTop || y > gridBottom)
                continue;

            Rect r(x, y, CELL_SIZE, CELL_SIZE);
            Image img(g_images[i].c_str());
            if (img.GetLastStatus() == Ok)
                g.DrawImage(&img, r);
        }

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    GdiplusStartupInput gd;
    GdiplusStartup(&g_gdiplusToken, &gd, nullptr);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = WINDOW_CLASS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    HWND hWnd = CreateWindowW(
        WINDOW_CLASS,
        WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW | WS_VSCROLL,
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
    return 0;
}
