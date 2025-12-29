#include "framework.h"
#include "app_state.h"
#include "filesystem.h"
#include "worker.h"
#include "input.h"

#include <cwctype>
#include <commdlg.h>

#include <algorithm>
#include <cmath>

static bool PtInRect(const RECT& r, int x, int y)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

static RECT MakeRect(int x, int y, int w, int h)
{
    RECT r{ x, y, x + w, y + h };
    return r;
}

static void EndEdit()
{
    g_editField = EditField::None;
    g_editBuf.clear();
}

void StartEdit(EditField f, const std::wstring& initial)
{
    g_editField = f;
    g_editBuf = initial;
}

void CancelEdit()
{
    EndEdit();
}

void CommitEdit()
{
    if (g_editField == EditField::None) return;

    switch (g_editField)
    {
    case EditField::MergeName:
        g_mergeName = g_editBuf;
        break;
    case EditField::RenPrefix:
        g_renPrefix = g_editBuf;
        break;
    case EditField::RenSuffix:
        g_renSuffix = g_editBuf;
        break;
    default:
        break;
    }

    EndEdit();
}

bool IsAllowedChar(wchar_t c)
{
    static const wchar_t* bad = L"<>:\"/\\|?*";
    if (c < 32) return false;
    if (wcschr(bad, c)) return false;
    if (c == L'\t' || c == L'\r' || c == L'\n') return false;
    return iswprint(c) != 0;
}

void HandleCharInput(HWND hWnd, wchar_t ch)
{
    if (g_editField == EditField::None) return;

    if (ch == 8)
    {
        if (!g_editBuf.empty()) g_editBuf.pop_back();
        InvalidateRect(hWnd, nullptr, FALSE);
        return;
    }

    if (ch == 13)
    {
        CommitEdit();
        InvalidateRect(hWnd, nullptr, FALSE);
        return;
    }

    if (!IsAllowedChar(ch)) return;

    g_editBuf.push_back(ch);
    InvalidateRect(hWnd, nullptr, FALSE);
}

static void GoHome()
{
    StopWorkerIfAny();
    ClearAllStateToHome();
    if (g_hWndMain) InvalidateRect(g_hWndMain, nullptr, TRUE);
}

static std::wstring PickImageFileDialog(HWND hWnd)
{
    wchar_t file[MAX_PATH] = L"";

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;

    ofn.lpstrFilter =
        L"Images (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0"
        L"All files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (!GetOpenFileNameW(&ofn)) return L"";
    return file;
}

void HitHomeClick(HWND hWnd, int mx, int my, const RECT& rc)
{
    int w = rc.right;

    int bw = 180;
    int gap = 24;
    int rowGap = 16;

    int totalW = bw * 2 + gap;
    int bx = (w - totalW) / 2;
    int by = 220;

    RECT b1 = MakeRect(bx, by, bw, 44);
    RECT b2 = MakeRect(bx + bw + gap, by, bw, 44);
    RECT b3 = MakeRect(bx, by + 44 + rowGap, bw, 44);
    RECT b4 = MakeRect(bx + bw + gap, by + 44 + rowGap, bw, 44);

    if (PtInRect(b1, mx, my))
    {
        g_tool = Tool::Denoise;
        ClearToolStateKeepTool();
        g_view = View::Pick;
        InvalidateRect(hWnd, nullptr, TRUE);
        return;
    }
    if (PtInRect(b2, mx, my))
    {
        g_tool = Tool::Merge;
        ClearToolStateKeepTool();
        g_view = View::Pick;
        InvalidateRect(hWnd, nullptr, TRUE);
        return;
    }
    if (PtInRect(b3, mx, my))
    {
        g_tool = Tool::Rename;
        ClearToolStateKeepTool();
        g_view = View::Pick;
        InvalidateRect(hWnd, nullptr, TRUE);
        return;
    }
    if (PtInRect(b4, mx, my))
    {
        g_tool = Tool::Crop;
        ClearToolStateKeepTool();
        g_view = View::Pick;
        InvalidateRect(hWnd, nullptr, TRUE);
        return;
    }
}

void HandlePickClick(HWND hWnd, int mx, int my, const RECT& rc)
{
    int w = rc.right;
    int h = rc.bottom;

    RECT backTop = MakeRect(PAD, 12, 42, 32);
    if (PtInRect(backTop, mx, my))
    {
        GoHome();
        return;
    }

    RECT choose = MakeRect(PAD + 58, 12, 160, 32);
    if (PtInRect(choose, mx, my))
    {
        if (g_tool == Tool::Crop)
        {
            std::wstring f = PickImageFileDialog(hWnd);
            if (!f.empty())
            {
                g_inputs.clear();
                g_thumbs.clear();

                g_inputs.push_back(f);
                g_thumbs.resize(1);
                g_thumbs[0].reset();

                SetStatus(L"Loaded");
                ResetScroll();
                InvalidateRect(hWnd, nullptr, TRUE);
            }
        }
        else
        {
            std::wstring folder = PickFolderDialog(hWnd, L"Выберите папку");
            if (!folder.empty())
            {
                LoadInputsFromFolder(folder);
                InvalidateRect(hWnd, nullptr, TRUE);
            }
        }
        return;
    }

    RECT btnBack = MakeRect(PAD, h - FOOTER_H + 6, 120, 32);
    RECT btnNext = MakeRect(w - PAD - 160, h - FOOTER_H + 6, 160, 32);

    if (PtInRect(btnBack, mx, my))
    {
        GoHome();
        return;
    }

    if (PtInRect(btnNext, mx, my))
    {
        bool ok = (g_tool == Tool::Crop) ? (g_inputs.size() == 1) : (!g_inputs.empty());
        if (ok)
        {
            g_view = View::Setup;
            EndEdit();

            if (g_tool == Tool::Crop)
            {
                g_cropGuides.clear();
                g_cropDragging = false;
                g_cropDragGuideIndex = -1;
                g_cropZoom = 1.0f;
                g_cropScrollY = 0;
                g_cropScrollMax = 0;

                g_cropSrcPath = g_inputs[0];
                g_cropBmp.reset(new Bitmap(g_inputs[0].c_str()));
            }

            InvalidateRect(hWnd, nullptr, TRUE);
        }
        return;
    }
}

static void StartProcessing(HWND hWnd)
{
    if (g_inputs.empty()) return;

    if (g_tool == Tool::Denoise)
    {
        if (g_outputFolder.empty()) return;
    }
    else if (g_tool == Tool::Merge)
    {
        if (g_mergeOutFolder.empty()) return;
        if (g_mergeName.empty()) g_mergeName = L"merged";
    }
    else if (g_tool == Tool::Rename)
    {
        if (g_outputFolder.empty()) return;
    }
    else if (g_tool == Tool::Crop)
    {
        if (g_outputFolder.empty()) return;
        if (g_inputs.size() != 1) return;
        if (!g_cropBmp || g_cropBmp->GetLastStatus() != Ok) return;
    }

    EndEdit();
    g_processing = true;
    g_processed = 0;

    if (g_tool == Tool::Crop)
    {
        int imgH = (g_cropBmp && g_cropBmp->GetLastStatus() == Ok) ? (int)g_cropBmp->GetHeight() : 0;
        g_total = max(0, CropGetSegmentCount(imgH));
    }
    else
    {
        g_total = (int)g_inputs.size();
    }

    g_elapsedMs = 0;
    g_view = View::Processing;

    InvalidateRect(hWnd, nullptr, TRUE);

    StartWorker(hWnd);
}

void HandleSetupClick(HWND hWnd, int mx, int my, const RECT& rc)
{
    int w = rc.right;
    int h = rc.bottom;

    RECT backTop = MakeRect(PAD, 12, 42, 32);
    if (PtInRect(backTop, mx, my))
    {
        EndEdit();
        g_view = View::Pick;
        InvalidateRect(hWnd, nullptr, TRUE);
        return;
    }

    int y = TOOLBAR_H + 28;

    RECT btnBack = MakeRect(PAD, h - FOOTER_H + 6, 120, 32);
    RECT btnStart = MakeRect(w - PAD - 160, h - FOOTER_H + 6, 160, 32);

    if (PtInRect(btnBack, mx, my))
    {
        EndEdit();
        g_view = View::Pick;
        InvalidateRect(hWnd, nullptr, TRUE);
        return;
    }

    if (g_tool == Tool::Denoise)
    {
        RECT outBox = MakeRect(PAD, y + 28, 420, 38);
        if (PtInRect(outBox, mx, my))
        {
            std::wstring folder = PickFolderDialog(hWnd, L"Папка для результата (denoise)");
            if (!folder.empty()) g_outputFolder = folder;
            InvalidateRect(hWnd, nullptr, TRUE);
            return;
        }

        int y2 = y + 92;
        RECT wMinus = MakeRect(PAD + 210 + 120 + 10, y2 - 2, 30, 28);
        RECT wPlus = MakeRect(PAD + 210 + 120 + 10 + 38, y2 - 2, 30, 28);

        if (PtInRect(wMinus, mx, my))
        {
            g_outWidth = max(600, g_outWidth - 100);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
        if (PtInRect(wPlus, mx, my))
        {
            g_outWidth = min(4000, g_outWidth + 100);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }

        int y3 = y2 + 52;
        RECT m1 = MakeRect(PAD, y3 + 28, 270, 32);
        RECT m2 = MakeRect(PAD, y3 + 72, 270, 32);
        RECT m3 = MakeRect(PAD, y3 + 116, 270, 32);

        if (PtInRect(m1, mx, my)) { g_dnMode = DenoiseMode::Manga; InvalidateRect(hWnd, nullptr, FALSE); return; }
        if (PtInRect(m2, mx, my)) { g_dnMode = DenoiseMode::Color; InvalidateRect(hWnd, nullptr, FALSE); return; }
        if (PtInRect(m3, mx, my)) { g_dnMode = DenoiseMode::Balanced; InvalidateRect(hWnd, nullptr, FALSE); return; }

        if (PtInRect(btnStart, mx, my))
        {
            if (!g_outputFolder.empty() && !g_inputs.empty())
                StartProcessing(hWnd);
            return;
        }
    }
    else if (g_tool == Tool::Merge)
    {
        RECT outBox = MakeRect(PAD, y + 28, 420, 38);
        if (PtInRect(outBox, mx, my))
        {
            std::wstring folder = PickFolderDialog(hWnd, L"Папка для результата (merge)");
            if (!folder.empty()) g_mergeOutFolder = folder;
            InvalidateRect(hWnd, nullptr, TRUE);
            return;
        }

        int y2 = y + 92;
        RECT nameBox = MakeRect(PAD + 120, y2 - 2, 260, 28);
        if (PtInRect(nameBox, mx, my))
        {
            StartEdit(EditField::MergeName, g_mergeName);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }

        int fy = y2 + 44;
        RECT fPng = MakeRect(PAD + 120, fy - 2, 100, 28);
        RECT fJpg = MakeRect(PAD + 230, fy - 2, 110, 28);

        if (PtInRect(fPng, mx, my)) { g_mergeJpeg = false; InvalidateRect(hWnd, nullptr, FALSE); return; }
        if (PtInRect(fJpg, mx, my)) { g_mergeJpeg = true;  InvalidateRect(hWnd, nullptr, FALSE); return; }

        if (PtInRect(btnStart, mx, my))
        {
            if (!g_mergeOutFolder.empty() && !g_inputs.empty())
                StartProcessing(hWnd);
            return;
        }
    }
    else if (g_tool == Tool::Crop)
    {
        RECT outBox = MakeRect(PAD, y + 28, 420, 38);
        if (PtInRect(outBox, mx, my))
        {
            std::wstring folder = PickFolderDialog(hWnd, L"Папка для результата (crop)");
            if (!folder.empty()) g_outputFolder = folder;
            InvalidateRect(hWnd, nullptr, TRUE);
            return;
        }

        int y2 = y + 92;
        RECT fPng = MakeRect(PAD + 120, y2 - 2, 100, 28);
        RECT fJpg = MakeRect(PAD + 230, y2 - 2, 110, 28);
        if (PtInRect(fPng, mx, my)) { g_cropJpeg = false; InvalidateRect(hWnd, nullptr, FALSE); return; }
        if (PtInRect(fJpg, mx, my)) { g_cropJpeg = true;  InvalidateRect(hWnd, nullptr, FALSE); return; }

        if (PtInRect(btnStart, mx, my))
        {
            if (!g_outputFolder.empty() && (g_inputs.size() == 1) && g_cropBmp && g_cropBmp->GetLastStatus() == Ok)
                StartProcessing(hWnd);
            return;
        }

        if (g_cropBmp && g_cropBmp->GetLastStatus() == Ok)
        {
            int previewTop = (y + 92) + 50;
            int previewBottom = h - FOOTER_H - 12;
            int previewH = max(0, previewBottom - previewTop);
            int previewW = w - PAD * 2;
            RECT pv = MakeRect(PAD, previewTop, previewW, previewH);

            if (PtInRect(pv, mx, my))
            {
                int imgW = (int)g_cropBmp->GetWidth();
                int imgH = (int)g_cropBmp->GetHeight();

                float baseScale = (float)previewW / (float)max(1, imgW);
                float scale = baseScale * max(0.05f, g_cropZoom);
                if (scale <= 0.0001f) scale = 0.0001f;

                int yImg = g_cropScrollY + (int)((float)(my - pv.top) / scale);
                yImg = ClampI(yImg, 0, imgH);

                // Near an existing guide? -> drag it. Otherwise add a new guide and drag it.
                int tolImg = max(2, (int)std::round(6.0f / scale));
                int bestIdx = -1;
                int bestDist = 0x7fffffff;

                for (int i = 0; i < (int)g_cropGuides.size(); ++i)
                {
                    int d = abs(g_cropGuides[(size_t)i] - yImg);
                    if (d <= tolImg && d < bestDist)
                    {
                        bestDist = d;
                        bestIdx = i;
                    }
                }

                if (bestIdx < 0)
                {
                    if (yImg > 0 && yImg < imgH)
                    {
                        g_cropGuides.push_back(yImg);
                        std::sort(g_cropGuides.begin(), g_cropGuides.end());
                        g_cropGuides.erase(std::unique(g_cropGuides.begin(), g_cropGuides.end()), g_cropGuides.end());
                        auto it = std::find(g_cropGuides.begin(), g_cropGuides.end(), yImg);
                        bestIdx = (it != g_cropGuides.end()) ? (int)std::distance(g_cropGuides.begin(), it) : -1;
                    }
                }

                if (bestIdx >= 0)
                {
                    g_cropDragging = true;
                    g_cropDragGuideIndex = bestIdx;
                    SetCapture(hWnd);
                }

                InvalidateRect(hWnd, nullptr, FALSE);
                return;
            }
        }

        return;
    }
    else
    {
        RECT outBox = MakeRect(PAD, y + 28, 420, 38);
        if (PtInRect(outBox, mx, my))
        {
            std::wstring folder = PickFolderDialog(hWnd, L"Папка для результата (rename)");
            if (!folder.empty()) g_outputFolder = folder;
            InvalidateRect(hWnd, nullptr, TRUE);
            return;
        }

        int y2 = y + 92;
        RECT pBox = MakeRect(PAD + 120, y2 - 2, 260, 28);
        if (PtInRect(pBox, mx, my))
        {
            StartEdit(EditField::RenPrefix, g_renPrefix);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }

        int y3 = y2 + 40;
        RECT sBox = MakeRect(PAD + 120, y3 - 2, 260, 28);
        if (PtInRect(sBox, mx, my))
        {
            StartEdit(EditField::RenSuffix, g_renSuffix);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }

        int y4 = y3 + 46;
        RECT nMinus = MakeRect(PAD + 160 + 120 + 10, y4 - 2, 30, 28);
        RECT nPlus = MakeRect(PAD + 160 + 120 + 10 + 38, y4 - 2, 30, 28);
        if (PtInRect(nMinus, mx, my)) { g_renStart = max(0, g_renStart - 1); InvalidateRect(hWnd, nullptr, FALSE); return; }
        if (PtInRect(nPlus, mx, my)) { g_renStart = g_renStart + 1;          InvalidateRect(hWnd, nullptr, FALSE); return; }

        int y5 = y4 + 46;
        RECT zMinus = MakeRect(PAD + 160 + 120 + 10, y5 - 2, 30, 28);
        RECT zPlus = MakeRect(PAD + 160 + 120 + 10 + 38, y5 - 2, 30, 28);
        if (PtInRect(zMinus, mx, my)) { g_renPad = max(1, g_renPad - 1); InvalidateRect(hWnd, nullptr, FALSE); return; }
        if (PtInRect(zPlus, mx, my)) { g_renPad = min(8, g_renPad + 1); InvalidateRect(hWnd, nullptr, FALSE); return; }

        if (PtInRect(btnStart, mx, my))
        {
            if (!g_outputFolder.empty() && !g_inputs.empty())
                StartProcessing(hWnd);
            return;
        }
    }
}

void HandleDoneClick(HWND hWnd, int mx, int my, const RECT& rc)
{
    int w = rc.right;
    int h = rc.bottom;

    RECT backTop = MakeRect(PAD, 12, 42, 32);
    if (PtInRect(backTop, mx, my))
    {
        GoHome();
        return;
    }

    RECT again = MakeRect(w / 2 - 170, h - FOOTER_H - 70, 160, 40);
    RECT home = MakeRect(w / 2 + 10, h - FOOTER_H - 70, 160, 40);

    if (PtInRect(again, mx, my))
    {
        StopWorkerIfAny();
        g_view = View::Pick;
        g_processing = false;
        g_processed = 0;
        g_total = 0;
        g_elapsedMs = 0;
        EndEdit();
        ResetScroll();
        InvalidateRect(hWnd, nullptr, TRUE);
        return;
    }

    if (PtInRect(home, mx, my))
    {
        GoHome();
        return;
    }
}


void HandleSetupMouseMove(HWND hWnd, int mx, int my, const RECT& rc)
{
    if (g_view != View::Setup || g_tool != Tool::Crop) return;
    if (!g_cropDragging) return;
    if (!g_cropBmp || g_cropBmp->GetLastStatus() != Ok) return;
    if (g_cropDragGuideIndex < 0 || g_cropDragGuideIndex >= (int)g_cropGuides.size()) return;

    int w = rc.right;
    int h = rc.bottom;

    int y = TOOLBAR_H + 28;
    int y2 = y + 92;

    int previewTop = y2 + 50;
    int previewBottom = h - FOOTER_H - 12;
    int previewH = max(0, previewBottom - previewTop);
    int previewW = w - PAD * 2;

    RECT pv{ PAD, previewTop, PAD + previewW, previewTop + previewH };

    int imgW = (int)g_cropBmp->GetWidth();
    int imgH = (int)g_cropBmp->GetHeight();
    float baseScale = (float)previewW / (float)max(1, imgW);
    float scale = baseScale * max(0.05f, g_cropZoom);
    if (scale <= 0.0001f) scale = 0.0001f;

    int yImg = g_cropScrollY + (int)((float)(my - pv.top) / scale);
    yImg = ClampI(yImg, 1, max(1, imgH - 1));

    // Keep ordering stable: do not allow a guide to cross its neighbors.
    int minY = 1;
    int maxY = max(1, imgH - 1);
    if (g_cropDragGuideIndex > 0) minY = g_cropGuides[(size_t)g_cropDragGuideIndex - 1] + 1;
    if (g_cropDragGuideIndex + 1 < (int)g_cropGuides.size()) maxY = g_cropGuides[(size_t)g_cropDragGuideIndex + 1] - 1;
    yImg = ClampI(yImg, minY, maxY);

    g_cropGuides[(size_t)g_cropDragGuideIndex] = yImg;
    InvalidateRect(hWnd, nullptr, FALSE);
}

void HandleSetupLButtonUp(HWND hWnd, int mx, int my, const RECT& rc)
{
    (void)mx; (void)my; (void)rc;
    if (g_view != View::Setup || g_tool != Tool::Crop) return;
    if (!g_cropDragging) return;

    g_cropDragging = false;
    g_cropDragGuideIndex = -1;
    ReleaseCapture();

    InvalidateRect(hWnd, nullptr, FALSE);
}

void HandleSetupRButtonDown(HWND hWnd, int mx, int my, const RECT& rc)
{
    if (g_view != View::Setup || g_tool != Tool::Crop) return;
    if (!g_cropBmp || g_cropBmp->GetLastStatus() != Ok) return;

    int w = rc.right;
    int h = rc.bottom;

    int y = TOOLBAR_H + 28;
    int y2 = y + 92;

    int previewTop = y2 + 50;
    int previewBottom = h - FOOTER_H - 12;
    int previewH = max(0, previewBottom - previewTop);
    int previewW = w - PAD * 2;

    RECT pv{ PAD, previewTop, PAD + previewW, previewTop + previewH };
    if (!PtInRect(pv, mx, my)) return;

    int imgW = (int)g_cropBmp->GetWidth();
    int imgH = (int)g_cropBmp->GetHeight();
    float baseScale = (float)previewW / (float)max(1, imgW);
    float scale = baseScale * max(0.05f, g_cropZoom);
    if (scale <= 0.0001f) scale = 0.0001f;

    int yImg = g_cropScrollY + (int)((float)(my - pv.top) / scale);
    yImg = ClampI(yImg, 0, imgH);

    int tolImg = max(2, (int)std::round(6.0f / scale));
    int bestIdx = -1;
    int bestDist = 0x7fffffff;
    for (int i = 0; i < (int)g_cropGuides.size(); i++)
    {
        int d = abs(g_cropGuides[(size_t)i] - yImg);
        if (d <= tolImg && d < bestDist)
        {
            bestDist = d;
            bestIdx = i;
        }
    }
    if (bestIdx >= 0)
    {
        g_cropGuides.erase(g_cropGuides.begin() + bestIdx);
        InvalidateRect(hWnd, nullptr, FALSE);
    }
}

void HandleSetupMouseWheel(HWND hWnd, short wheelDelta, int mx, int my, const RECT& rc)
{
    if (g_view != View::Setup || g_tool != Tool::Crop) return;
    if (!g_cropBmp || g_cropBmp->GetLastStatus() != Ok) return;

    int w = rc.right;
    int h = rc.bottom;

    int y = TOOLBAR_H + 28;
    int y2 = y + 92;

    int previewTop = y2 + 50;
    int previewBottom = h - FOOTER_H - 12;
    int previewH = max(0, previewBottom - previewTop);
    int previewW = w - PAD * 2;

    int imgW = (int)g_cropBmp->GetWidth();
    int imgH = (int)g_cropBmp->GetHeight();

    float baseScale = (float)previewW / (float)max(1, imgW);
    float scale = baseScale * max(0.05f, g_cropZoom);
    if (scale <= 0.0001f) scale = 0.0001f;

    // Ctrl+wheel = zoom; plain wheel = scroll.
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    if (ctrl)
    {
        // Keep the image Y under the cursor stable while zooming.
        RECT pv{ PAD, previewTop, PAD + previewW, previewTop + previewH };
        int yImgAtMouse = g_cropScrollY + (int)((float)(my - pv.top) / scale);

        float step = (wheelDelta > 0) ? 1.1f : (1.0f / 1.1f);
        float newZoom = g_cropZoom * step;
        newZoom = max(0.25f, min(8.0f, newZoom));

        float newScale = baseScale * max(0.05f, newZoom);
        if (newScale <= 0.0001f) newScale = 0.0001f;

        int newScroll = yImgAtMouse - (int)((float)(my - pv.top) / newScale);
        g_cropZoom = newZoom;

        int newVisibleImgH = (int)((float)previewH / newScale);
        if (newVisibleImgH < 1) newVisibleImgH = 1;
        g_cropScrollMax = max(0, imgH - newVisibleImgH);
        g_cropScrollY = ClampI(newScroll, 0, g_cropScrollMax);
    }
    else
    {
        int visibleImgH = (int)((float)previewH / scale);
        if (visibleImgH < 1) visibleImgH = 1;
        g_cropScrollMax = max(0, imgH - visibleImgH);

        int delta = (wheelDelta / 120) * 180;
        g_cropScrollY = ClampI(g_cropScrollY - delta, 0, g_cropScrollMax);
    }

    InvalidateRect(hWnd, nullptr, FALSE);
}
