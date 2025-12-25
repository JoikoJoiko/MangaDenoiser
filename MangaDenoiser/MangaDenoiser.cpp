#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <filesystem>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;
namespace fs = std::filesystem;

constexpr wchar_t WINDOW_CLASS[] = L"MangaDenoiserWindow";
constexpr wchar_t WINDOW_TITLE[] = L"Manga Denoiser";

ULONG_PTR g_gdiplusToken;

std::wstring g_status = L"Ready";
std::vector<std::wstring> g_images;

bool IsImageFile(const fs::path& p)
{
    if (!p.has_extension()) return false;
    auto ext = p.extension().wstring();
    for (auto& c : ext) c = towlower(c);

    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" ||
        ext == L".bmp" || ext == L".webp";
}

void LoadImagesFromPath(const std::wstring& path)
{
    g_images.clear();

    fs::path p(path);

    if (fs::is_regular_file(p))
    {
        if (IsImageFile(p))
            g_images.push_back(p.wstring());
    }
    else if (fs::is_directory(p))
    {
        for (auto& entry : fs::recursive_directory_iterator(p))
        {
            if (entry.is_regular_file() && IsImageFile(entry.path()))
            {
                g_images.push_back(entry.path().wstring());
            }
        }
    }

    if (!g_images.empty())
        g_status = L"Loaded: " + std::to_wstring(g_images.size()) + L" images";
    else
        g_status = L"No images found";
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        DragAcceptFiles(hWnd, TRUE);
        HBRUSH brush = CreateSolidBrush(RGB(30, 30, 30));
        SetClassLongPtr(hWnd, GCLP_HBRBACKGROUND, (LONG_PTR)brush);
        return 0;
    }

    case WM_DROPFILES:
    {
        HDROP hDrop = (HDROP)wParam;
        wchar_t path[MAX_PATH];

        if (DragQueryFile(hDrop, 0, path, MAX_PATH))
        {
            LoadImagesFromPath(path);
            InvalidateRect(hWnd, nullptr, TRUE);
        }

        DragFinish(hDrop);
        return 0;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rc;
        GetClientRect(hWnd, &rc);

        Graphics graphics(hdc);
        graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);

        if (!g_images.empty())
        {
            Image image(g_images[0].c_str());

            if (image.GetLastStatus() == Ok)
            {
                int imgW = image.GetWidth();
                int imgH = image.GetHeight();

                float scale = min(
                    (float)rc.right / imgW,
                    (float)(rc.bottom - 40) / imgH
                );

                int drawW = (int)(imgW * scale);
                int drawH = (int)(imgH * scale);

                int x = (rc.right - drawW) / 2;
                int y = (rc.bottom - drawH) / 2 - 10;

                graphics.DrawImage(&image, x, y, drawW, drawH);
            }
        }
        else
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

            DrawTextW(
                hdc,
                L"Drop images or a folder here",
                -1,
                &rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE
            );

            SelectObject(hdc, oldFont);
            DeleteObject(hFont);
        }

        RECT statusRc = rc;
        statusRc.top = rc.bottom - 35;

        SetTextColor(hdc, RGB(180, 180, 180));

        HFONT hStatusFont = CreateFontW(
            16, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            VARIABLE_PITCH,
            L"Segoe UI"
        );

        HFONT oldStatusFont = (HFONT)SelectObject(hdc, hStatusFont);

        std::wstring statusText = L"Status: " + g_status;
        DrawTextW(
            hdc,
            statusText.c_str(),
            -1,
            &statusRc,
            DT_LEFT | DT_VCENTER
        );

        SelectObject(hdc, oldStatusFont);
        DeleteObject(hStatusFont);

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr);

    WNDCLASS wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = WINDOW_CLASS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    RegisterClass(&wc);

    HWND hWnd = CreateWindowExW(
        0,
        WINDOW_CLASS,
        WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        900,
        600,
        nullptr,
        nullptr,
        hInstance,
        nullptr
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
