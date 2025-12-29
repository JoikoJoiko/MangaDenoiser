#include "framework.h"
#include "app_state.h"
#include "filesystem.h"
#include "tooltip.h"
#include "ui_views.h"
#include "input.h"
#include "worker.h"

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

        if (g_tool == Tool::Crop && g_inputs.size() > 1)
        {
            g_inputs.resize(1);
            g_thumbs.resize(1);
            if (!g_thumbs.empty()) g_thumbs[0].reset();
        }

        InvalidateRect(hWnd, nullptr, TRUE);
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        short d = GET_WHEEL_DELTA_WPARAM(wParam);

        if (g_view == View::Pick && !g_inputs.empty())
        {
            g_scrollTarget -= (d / 120) * 160;
            RECT rc; GetClientRect(hWnd, &rc);
            ComputeScrollMax(rc);
            g_scrollTarget = ClampI(g_scrollTarget, 0, g_scrollMax);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }

        if (g_view == View::Setup && g_tool == Tool::Crop)
        {
            RECT rc; GetClientRect(hWnd, &rc);
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            // For WM_MOUSEWHEEL, lParam is in screen coordinates.
            ScreenToClient(hWnd, &pt);
            HandleSetupMouseWheel(hWnd, d, pt.x, pt.y, rc);
            return 0;
        }

        return 0;
    }

    case WM_MOUSEMOVE:
    {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        UpdateTooltipByMouse(mx, my);

        if (g_view == View::Setup && g_tool == Tool::Crop)
        {
            RECT rc; GetClientRect(hWnd, &rc);
            HandleSetupMouseMove(hWnd, mx, my, rc);
        }
        return 0;
    }

    case WM_CHAR:
        HandleCharInput(hWnd, (wchar_t)wParam);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE && g_editField != EditField::None) { CancelEdit(); InvalidateRect(hWnd, nullptr, FALSE); return 0; }
        if (wParam == VK_RETURN && g_editField != EditField::None) { CommitEdit(); InvalidateRect(hWnd, nullptr, FALSE); return 0; }

        if (g_view == View::Setup && g_tool == Tool::Crop)
        {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (ctrl && wParam == 'Z')
            {
                if (!g_cropGuides.empty()) g_cropGuides.pop_back();
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }

            if (ctrl && (wParam == VK_OEM_PLUS || wParam == VK_ADD))
            {
                g_cropZoom = max(0.25f, min(8.0f, g_cropZoom * 1.1f));
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
            if (ctrl && (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT))
            {
                g_cropZoom = max(0.25f, min(8.0f, g_cropZoom / 1.1f));
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
            if (ctrl && (wParam == '0' || wParam == VK_NUMPAD0))
            {
                g_cropZoom = 1.0f;
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
        }

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

    case WM_LBUTTONUP:
    {
        if (g_view == View::Setup && g_tool == Tool::Crop)
        {
            RECT rc; GetClientRect(hWnd, &rc);
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);
            HandleSetupLButtonUp(hWnd, mx, my, rc);
            return 0;
        }
        return 0;
    }

    case WM_RBUTTONDOWN:
    {
        if (g_view == View::Setup && g_tool == Tool::Crop)
        {
            RECT rc; GetClientRect(hWnd, &rc);
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);
            HandleSetupRButtonDown(hWnd, mx, my, rc);
            return 0;
        }
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
