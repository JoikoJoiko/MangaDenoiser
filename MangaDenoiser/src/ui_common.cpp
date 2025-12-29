#include "framework.h"
#include "ui_common.h"

#include <cmath>

void DrawTextG(Graphics& g, const std::wstring& text, float x, float y, float w, float h, float size, Color color, bool bold, int alignH)
{
    FontFamily ff(L"Segoe UI");
    Font f(&ff, size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    SolidBrush b(color);
    RectF r(x, y, w, h);
    StringFormat sf;
    sf.SetLineAlignment(StringAlignmentCenter);
    if (alignH < 0) sf.SetAlignment(StringAlignmentNear);
    else if (alignH > 0) sf.SetAlignment(StringAlignmentFar);
    else sf.SetAlignment(StringAlignmentCenter);
    g.DrawString(text.c_str(), -1, &f, r, &sf, &b);
}

void RoundedPath(GraphicsPath& path, float x, float y, float w, float h, float r)
{
    float d = r * 2.f;
    path.Reset();
    path.AddArc(x, y, d, d, 180.f, 90.f);
    path.AddArc(x + w - d, y, d, d, 270.f, 90.f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.f, 90.f);
    path.AddArc(x, y + h - d, d, d, 90.f, 90.f);
    path.CloseFigure();
}

void FillRoundRect(Graphics& g, const RectF& r, float radius, Brush& fill, Pen* border)
{
    GraphicsPath path;
    RoundedPath(path, r.X, r.Y, r.Width, r.Height, radius);
    g.FillPath(&fill, &path);
    if (border) g.DrawPath(border, &path);
}

void DrawButton(Graphics& g, const RectI& r, const wchar_t* text, bool disabled, bool accentLine)
{
    SolidBrush bg(disabled ? C_BTN_DIS : C_BTN);
    Pen br(C_BORDER, 1.0f);
    g.FillRectangle(&bg, r.x, r.y, r.w, r.h);
    g.DrawRectangle(&br, r.x, r.y, r.w, r.h);

    if (!disabled && accentLine)
    {
        Pen acc(C_ACC, 2.0f);
        g.DrawLine(&acc, r.x, r.y + r.h - 1, r.x + r.w, r.y + r.h - 1);
    }

    DrawTextG(g, text, (float)r.x, (float)r.y, (float)r.w, (float)r.h, 14.0f,
        disabled ? Color(255, 135, 135, 135) : C_TEXT, false, 0);
}

void DrawBackArrow(Graphics& g, const RectI& r)
{
    SolidBrush bg(Color(255, 48, 48, 48));
    Pen br(C_BORDER, 1.0f);
    g.FillRectangle(&bg, r.x, r.y, r.w, r.h);
    g.DrawRectangle(&br, r.x, r.y, r.w, r.h);

    Pen acc(C_ACC, 2.0f);
    int cx = r.x + r.w / 2;
    int cy = r.y + r.h / 2;
    g.DrawLine(&acc, cx + 6, cy - 9, cx - 6, cy);
    g.DrawLine(&acc, cx - 6, cy, cx + 6, cy + 9);
    g.DrawLine(&acc, cx - 6, cy, cx + 10, cy);
}

void DrawInfoIcon(Graphics& g, const RectI& r)
{
    SolidBrush bg(Color(255, 40, 40, 40));
    Pen br(C_BORDER, 1.0f);
    Pen acc(C_ACC, 2.0f);
    SolidBrush dot(C_ACC);

    g.FillEllipse(&bg, r.x, r.y, r.w, r.h);
    g.DrawEllipse(&br, r.x, r.y, r.w, r.h);

    int cx = r.x + r.w / 2;
    int cy = r.y + r.h / 2;

    g.DrawLine(&acc, cx, cy - 6, cx, cy + 3);
    g.FillEllipse(&dot, cx - 2, cy + 6, 4, 4);
}

void DrawWindowAccent(Graphics& g, int w, int h)
{
    Pen acc(C_ACC, 2.0f);
    g.DrawRectangle(&acc, 1, 1, w - 3, h - 3);
}

void DrawToolbarCommon(Graphics& g, int w, bool showBackToHome)
{
    SolidBrush tb(C_PANEL);
    g.FillRectangle(&tb, 0, 0, w, TOOLBAR_H);

    Pen acc(C_ACC, 2.f);
    g.DrawLine(&acc, 0, TOOLBAR_H - 1, w, TOOLBAR_H - 1);

    if (showBackToHome)
    {
        RectI back{ PAD, 12, 42, 32 };
        DrawBackArrow(g, back);
    }
}

void DrawFooter(Graphics& g, int w, int h, bool showBack, bool showNext, const std::wstring& nextText, bool nextDisabled)
{
    SolidBrush fb(C_PANEL);
    g.FillRectangle(&fb, 0, h - FOOTER_H, w, FOOTER_H);

    Pen acc(C_ACC, 2.f);
    g.DrawLine(&acc, 0, h - FOOTER_H, w, h - FOOTER_H);

    RectI btnBack{ PAD, h - FOOTER_H + 6, 120, 32 };
    RectI btnNext{ w - PAD - 160, h - FOOTER_H + 6, 160, 32 };

    int leftX = PAD;

    if (showBack)
    {
        DrawButton(g, btnBack, L"Back", false);
        leftX = btnBack.x + btnBack.w + 16;
    }

    if (showNext)
    {
        DrawButton(g, btnNext, nextText.c_str(), nextDisabled);
    }

    std::wstring line;
    if (g_tool == Tool::Crop && g_view == View::Setup && g_cropBmp && g_cropBmp->GetLastStatus() == Ok)
    {
        int imgH = (int)g_cropBmp->GetHeight();
        std::vector<int> guides;
        CropGetSortedGuides(imgH, guides);
        int segments = max(0, (int)guides.size() + 1);
        int zoomPct = (int)std::round(g_cropZoom * 100.0f);
        line = L"Segments: " + std::to_wstring(segments) + L"    Guides: " + std::to_wstring((int)guides.size()) +
            L"    Zoom: " + std::to_wstring(zoomPct) + L"%    Status: " + g_status;
    }
    else
    {
        line = L"Items: " + std::to_wstring((int)g_inputs.size()) + L"    Status: " + g_status;
    }
    DrawTextG(g, line, (float)leftX, (float)(h - FOOTER_H),
        (float)(w - leftX - PAD - (showNext ? (btnNext.w + 16) : 0)),
        (float)FOOTER_H, 13.f, C_SUB, false, -1);
}
