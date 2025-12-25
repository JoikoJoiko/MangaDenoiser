#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

constexpr wchar_t WINDOW_CLASS[] = L"MangaDenoiserWindow";
constexpr wchar_t WINDOW_TITLE[] = L"Manga Denoiser";

ULONG_PTR g_gdiplusToken;

std::vector<std::wstring> g_images;
int g_currentIndex = -1;

std::wstring g_status = L"Ready";

bool IsImageFile(const std::wstring& path)
{
    auto ext = path.substr(path.find_last_of(L'.') + 1);
    for (auto& c : ext) c = towlower(c);
    return ext == L"png" || ext == L"jpg" || ext == L"jpeg" || ext == L"bmp";
}

void AddImagesFromDrop(HDROP hDrop)
{
    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    wchar_t path[MAX_PATH];

    for (UINT i = 0; i < count; i++)
    {
        DragQueryFileW(hDrop, i, path, MAX_PATH);
        std::wstring p = path;

        DWORD attr = GetFileAttributesW(p.c_str());
        if (attr & FILE_ATTRIBUTE_DIRECTORY)
        {
            WIN32_FIND_DATAW fd;
            std::wstring mask = p + L"\\*.*";
            HANDLE hFind = FindFirstFileW(mask.c_str(), &fd);
            if (hFind != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    {
                        std::wstring file = p + L"\\" + fd.cFileName;
                        if (IsImageFile(file))
                            g_images.push_back(file);
                    }
                } while (FindNextFileW(hFind, &fd));
                FindClose(hFind);
            }
        }
        else if (IsImageFile(p))
        {
            g_images.push_back(p);
        }
    }

    if (!g_images.empty())
    {
        g_currentIndex = 0;
        g_status = L"Loaded " + std::to_wstring(g_images.size()) + L" images";
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        DragAcceptFiles(hWnd, TRUE);
        SetClassLongPtr(hWnd, GCLP_HBRBACKGROUND,
            (LONG_PTR)CreateSolidBrush(RGB(30, 30, 30)));
        return 0;

    case WM_DROPFILES:
        g_images.clear();
        g_currentIndex = -1;
        AddImagesFromDrop((HDROP)wParam);
        DragFinish((HDROP)wParam);
        InvalidateRect(hWnd, nullptr, TRUE);
        return 0;

    case WM_KEYDOWN:
        if (!g_images.empty())
        {
            if (wParam == VK_RIGHT && g_currentIndex < (int)g_images.size() - 1)
                g_currentIndex++;
            if (wParam == VK_LEFT && g_currentIndex > 0)
                g_currentIndex--;
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

        if (g_currentIndex >= 0)
        {
            Image img(g_images[g_currentIndex].c_str());
            if (img.GetLastStatus() == Ok)
            {
                float scale = min(
                    (float)rc.right / img.GetWidth(),
                    (float)(rc.bottom - 60) / img.GetHeight()
                );

                int w = (int)(img.GetWidth() * scale);
                int h = (int)(img.GetHeight() * scale);
                int x = (rc.right - w) / 2;
                int y = (rc.bottom - h) / 2 - 20;

                g.DrawImage(&img, x, y, w, h);
            }
        }
        else
        {
            SetTextColor(hdc, RGB(220, 220, 220));
            SetBkMode(hdc, TRANSPARENT);
            DrawTextW(
                hdc,
                L"Drop images or folder here",
                -1,
                &rc,
                DT_CENTER | DT_VCENTER
            );
        }

        RECT statusRc = rc;
        statusRc.top = rc.bottom - 40;

        std::wstring info;
        if (g_currentIndex >= 0)
        {
            info =
                L"Image " +
                std::to_wstring(g_currentIndex + 1) +
                L" / " +
                std::to_wstring(g_images.size());
        }
        else
        {
            info = g_status;
        }

        SetTextColor(hdc, RGB(180, 180, 180));
        DrawTextW(hdc, info.c_str(), -1, &statusRc, DT_LEFT | DT_VCENTER);

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
        900, 600,
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
