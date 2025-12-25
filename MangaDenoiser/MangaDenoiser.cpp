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

constexpr int SIDEBAR_WIDTH = 160;
constexpr int THUMB_SIZE = 120;
constexpr int THUMB_PADDING = 10;

ULONG_PTR g_gdiplusToken;

std::vector<std::wstring> g_images;
int g_currentIndex = -1;
int g_hoverIndex = -1;

bool IsImageFile(const std::wstring& path)
{
    auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;

    std::wstring ext = path.substr(dot + 1);
    for (auto& c : ext) c = (wchar_t)towlower(c);

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
        {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;

            continue;
        }

        std::wstring f = folder + L"\\" + fd.cFileName;
        if (IsImageFile(f))
            g_images.push_back(f);

    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

void AddImagesFromDrop(HDROP hDrop)
{
    g_images.clear();
    g_currentIndex = -1;
    g_hoverIndex = -1;

    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    wchar_t path[MAX_PATH];

    for (UINT i = 0; i < count; i++)
    {
        if (!DragQueryFileW(hDrop, i, path, MAX_PATH))
            continue;

        std::wstring p = path;

        DWORD attr = GetFileAttributesW(p.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES)
            continue;

        if (attr & FILE_ATTRIBUTE_DIRECTORY)
        {
            AddImagesFromFolder(p);
        }
        else
        {
            if (IsImageFile(p))
                g_images.push_back(p);
        }
    }

    if (!g_images.empty())
        g_currentIndex = 0;
}

int HitTestThumbnail(int x, int y, int clientHeight)
{
    if (x >= SIDEBAR_WIDTH)
        return -1;

    int y0 = THUMB_PADDING;
    if (y < y0) return -1;

    int rowH = THUMB_SIZE + THUMB_PADDING;
    int index = (y - y0) / rowH;

    if (index < 0 || index >= (int)g_images.size())
        return -1;

    int top = y0 + index * rowH;
    int bottom = top + THUMB_SIZE;
    if (y > bottom) return -1;

    int visibleBottom = clientHeight - THUMB_PADDING;
    if (top > visibleBottom) return -1;

    return index;
}

void DrawCenteredText(HDC hdc, const RECT& rc, const wchar_t* text)
{
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(220, 220, 220));

    HFONT hFont = CreateFontW(
        28, 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        VARIABLE_PITCH,
        L"Segoe UI"
    );

    HFONT oldFont = (HFONT)SelectObject(hdc, hFont);

    RECT r = rc;
    r.left += SIDEBAR_WIDTH;
    DrawTextW(hdc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_WORDBREAK);

    SelectObject(hdc, oldFont);
    DeleteObject(hFont);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        DragAcceptFiles(hWnd, TRUE);
        SetClassLongPtr(hWnd, GCLP_HBRBACKGROUND, (LONG_PTR)CreateSolidBrush(RGB(25, 25, 25)));
        return 0;

    case WM_DROPFILES:
        AddImagesFromDrop((HDROP)wParam);
        DragFinish((HDROP)wParam);
        InvalidateRect(hWnd, nullptr, TRUE);
        return 0;

    case WM_LBUTTONDOWN:
    {
        RECT rc;
        GetClientRect(hWnd, &rc);

        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        int hit = HitTestThumbnail(x, y, rc.bottom);

        if (hit != -1)
        {
            g_currentIndex = hit;
            InvalidateRect(hWnd, nullptr, TRUE);
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (!g_images.empty() && g_currentIndex >= 0)
        {
            if (wParam == VK_RIGHT && g_currentIndex < (int)g_images.size() - 1) g_currentIndex++;
            if (wParam == VK_LEFT && g_currentIndex > 0) g_currentIndex--;
            InvalidateRect(hWnd, nullptr, TRUE);
        }
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rc;
        GetClientRect(hWnd, &rc);

        Graphics g(hdc);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

        SolidBrush sidebarBrush(Color(255, 32, 32, 32));
        g.FillRectangle(&sidebarBrush, 0, 0, SIDEBAR_WIDTH, rc.bottom);

        if (g_images.empty())
        {
            DrawCenteredText(hdc, rc, L"Drop images or a folder here");
            EndPaint(hWnd, &ps);
            return 0;
        }

        int rowH = THUMB_SIZE + THUMB_PADDING;
        int maxVisible = (rc.bottom - THUMB_PADDING) / rowH;
        if (maxVisible < 0) maxVisible = 0;

        int drawCount = (int)g_images.size();
        if (drawCount > maxVisible) drawCount = maxVisible;

        for (int i = 0; i < drawCount; i++)
        {
            int y = THUMB_PADDING + i * rowH;

            Rect thumbRect(THUMB_PADDING, y, THUMB_SIZE, THUMB_SIZE);

            Image img(g_images[i].c_str());
            if (img.GetLastStatus() == Ok)
                g.DrawImage(&img, thumbRect);

            if (i == g_currentIndex)
            {
                Pen pen(Color(255, 180, 120, 255), 2);
                g.DrawRectangle(&pen, thumbRect);
            }
        }

        if (g_currentIndex >= 0 && g_currentIndex < (int)g_images.size())
        {
            Image img(g_images[g_currentIndex].c_str());
            if (img.GetLastStatus() == Ok)
            {
                int availW = rc.right - SIDEBAR_WIDTH;
                int availH = rc.bottom;

                float scale = min((float)availW / img.GetWidth(), (float)availH / img.GetHeight());
                if (scale <= 0) scale = 1;

                int w = (int)(img.GetWidth() * scale);
                int h = (int)(img.GetHeight() * scale);

                int x = SIDEBAR_WIDTH + (availW - w) / 2;
                int y = (availH - h) / 2;

                g.DrawImage(&img, x, y, w, h);
            }
            else
            {
                DrawCenteredText(hdc, rc, L"Failed to load selected image");
            }
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
        1100, 700,
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
