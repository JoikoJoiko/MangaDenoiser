#include "framework.h"
#include "tooltip.h"
#include "ui_common.h"

void DrawTooltip(Graphics& g, int clientW, int clientH)
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

void UpdateTooltipByMouse(int mx, int my)
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
