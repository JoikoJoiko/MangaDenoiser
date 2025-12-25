#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

constexpr wchar_t WINDOW_CLASS[] = L"MangaDenoiserWindow";
constexpr wchar_t WINDOW_TITLE[] = L"Manga Denoiser";

constexpr int TOPBAR_HEIGHT = 48;
constexpr int BOTBAR_HEIGHT = 56;
constexpr int THUMB_SIZE = 140;
constexpr int THUMB_PADDING = 16;

ULONG_PTR g_gdiplusToken;
std::vector<std::wstring> g_images;

bool IsImageFile(const std::wstring& path)
{
    auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;

    std::wstring ext = path.substr(dot + 1);
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
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        std::wstring f = folder + L"\\" + fd.cFileName;
        if (IsImageFile(f))
            g_images.push_back(f);

    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

void AddImagesFromDrop(HDROP hDrop)
{
    g_images.clear();

    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    wchar_t path[MAX_PATH];

    for (UINT i = 0; i < count; i++)
    {
        DragQueryFileW(hDrop, i, path, MAX_PATH);
        std::wstring p = path;

        DWORD attr = GetFileAttributesW(p.c_str());
        if (attr & FILE_ATTRIBUTE_DIRECTORY)
            AddImagesFromFolder(p);
        else if (IsImageFile(p))
            g_images.push_back(p);
    }
}

void DrawCenteredText(Graphics& g, RectF rc, const wchar_t* text, float size)
{
    Font font(L"Segoe UI", size, FontStyleRegular);
    SolidBrush brush(Color(255, 220, 220, 220));
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);

    g.DrawString(text, -1, &font, rc, &fmt, &brush);
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
        InvalidateRect(hWnd, nullptr, TRUE);
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        Graphics g(hdc);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

        // Top bar
        SolidBrush top(Color(255, 32, 32, 32));
        g.FillRectangle(&top, 0, 0, rc.right, TOPBAR_HEIGHT);

        wchar_t title[128];
        swprintf_s(title, L"Manga Denoiser — Images: %d", (int)g_images.size());
        DrawCenteredText(
            g,
            RectF(0, 0, (REAL)rc.right, (REAL)TOPBAR_HEIGHT),
            title,
            16
        );

        // Bottom bar
        SolidBrush bot(Color(255, 32, 32, 32));
        g.FillRectangle(&bot, 0, rc.bottom - BOTBAR_HEIGHT, rc.right, BOTBAR_HEIGHT);

        DrawCenteredText(
            g,
            RectF(0, rc.bottom - BOTBAR_HEIGHT, (REAL)rc.right, (REAL)BOTBAR_HEIGHT),
            L"[ Choose Folder ]    [ Denoise ]    [ Save To... ]",
            14
        );

        // Content area
        int contentTop = TOPBAR_HEIGHT;
        int contentBottom = rc.bottom - BOTBAR_HEIGHT;

        if (g_images.empty())
        {
            DrawCenteredText(
                g,
                RectF(0, contentTop, (REAL)rc.right, (REAL)(contentBottom - contentTop)),
                L"Drop images or a folder here",
                24
            );
            EndPaint(hWnd, &ps);
            return 0;
        }

        int cols = max(1, rc.right / (THUMB_SIZE + THUMB_PADDING));
        int x0 = THUMB_PADDING;
        int y0 = contentTop + THUMB_PADDING;

        for (size_t i = 0; i < g_images.size(); i++)
        {
            int col = i % cols;
            int row = i / cols;

            int x = x0 + col * (THUMB_SIZE + THUMB_PADDING);
            int y = y0 + row * (THUMB_SIZE + THUMB_PADDING);

            if (y + THUMB_SIZE > contentBottom)
                break;

            Image img(g_images[i].c_str());
            if (img.GetLastStatus() == Ok)
                g.DrawImage(&img, x, y, THUMB_SIZE, THUMB_SIZE);
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
    return 0;
}
