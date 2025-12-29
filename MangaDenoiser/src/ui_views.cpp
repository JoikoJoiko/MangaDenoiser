#define NOMINMAX
#include <windows.h>

#include <gdiplus.h>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>

using namespace Gdiplus;
using std::min;
using std::max;

struct RectI { int x, y, w, h; };
static inline int ClampI(int v, int a, int b) { return (v < a) ? a : (v > b) ? b : v; }
static inline int iabs(int v) { return v < 0 ? -v : v; }

enum class View { Home, Pick, Setup, Processing, Done };
enum class Tool { None, Denoise, Merge, Rename };
enum class DenoiseMode { Manga, Color, Balanced };
enum class EditField { None, MergeName, RenPrefix, RenSuffix };

extern const int TOOLBAR_H;
extern const int FOOTER_H;
extern const int PAD;
extern const int CELL;
extern const int GAP;

extern View g_view;
extern Tool g_tool;

extern std::vector<std::wstring> g_inputs;
extern std::vector<std::unique_ptr<Bitmap>> g_thumbs;

extern std::wstring g_outputFolder;

extern std::wstring g_status;

extern int g_scrollY;
extern int g_scrollTarget;
extern int g_scrollMax;

extern int g_animTick;

extern bool g_processing;
extern int g_processed;
extern int g_total;
extern long long g_elapsedMs;

extern int g_outWidth;
extern DenoiseMode g_dnMode;

extern std::wstring g_mergeOutFolder;
extern bool g_mergeJpeg;
extern std::wstring g_mergeName;

extern std::wstring g_renPrefix;
extern std::wstring g_renSuffix;
extern int g_renStart;
extern int g_renPad;

extern bool g_ttShow;
extern std::wstring g_ttText;
extern POINT g_ttPos;

extern EditField g_editField;
extern std::wstring g_editBuf;

extern Color C_BG;
extern Color C_PANEL;
extern Color C_BTN;
extern Color C_BTN_DIS;
extern Color C_BORDER;
extern Color C_TEXT;
extern Color C_SUB;
extern Color C_ACC;

extern void EnsureThumb(size_t i);

extern int ComputeCols(int clientW);
extern int ComputeContentHeight(int cols, int count);

extern std::wstring EllipsizePath(const std::wstring& s, int maxChars);
extern std::wstring PadNumber(int v, int width);
extern std::wstring FormatDuration(long long ms);

extern void DrawTextG(Graphics& g,
    const std::wstring& text,
    float x, float y, float w, float h,
    float size,
    Color color,
    bool bold,
    int alignH);

extern void RoundedPath(GraphicsPath& path, float x, float y, float w, float h, float r);
extern void FillRoundRect(Graphics& g, const RectF& r, float radius, Brush& fill, Pen* border);

extern void DrawButton(Graphics& g, const RectI& r, const wchar_t* text, bool disabled, bool accentLine /*=true*/);
extern void DrawBackArrow(Graphics& g, const RectI& r);
extern void DrawInfoIcon(Graphics& g, const RectI& r);
extern void DrawWindowAccent(Graphics& g, int w, int h);

static void DrawTooltip(Graphics& g, int clientW, int clientH)
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

static void DrawToolbarCommon(Graphics& g, int w, bool showBackToHome)
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

static void DrawFooter(Graphics& g, int w, int h, bool showBack, bool showNext, const std::wstring& nextText, bool nextDisabled)
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
        DrawButton(g, btnBack, L"Back", false, true);
        leftX = btnBack.x + btnBack.w + 16;
    }

    if (showNext)
    {
        DrawButton(g, btnNext, nextText.c_str(), nextDisabled, true);
    }

    std::wstring line = L"Items: " + std::to_wstring((int)g_inputs.size()) + L"    Status: " + g_status;
    DrawTextG(
        g,
        line,
        (float)leftX,
        (float)(h - FOOTER_H),
        (float)(w - leftX - PAD - (showNext ? (btnNext.w + 16) : 0)),
        (float)FOOTER_H,
        13.f,
        C_SUB,
        false,
        -1
    );
}

static void DrawPeekingCat(Graphics& g, int w, int h, int t)
{
    int baseY = h - 12;
    int cx = w / 2;

    int eyeShift = (int)(std::sin((double)t * 0.08) * 6.0);
    int earWig = (int)(std::sin((double)t * 0.06) * 3.0);

    SolidBrush face(Color(255, 52, 52, 52));
    Pen out(Color(255, 92, 92, 92), 2.f);
    Pen whisk(C_SUB, 2.f);

    RectF head((REAL)(cx - 170), (REAL)(baseY - 120), 340.f, 160.f);
    GraphicsPath path;
    RoundedPath(path, head.X, head.Y, head.Width, head.Height, 56.f);
    g.FillPath(&face, &path);
    g.DrawPath(&out, &path);

    Point earL[3] = { Point(cx - 110, baseY - 118), Point(cx - 155, baseY - 162 - earWig), Point(cx - 70, baseY - 145) };
    Point earR[3] = { Point(cx + 110, baseY - 118), Point(cx + 155, baseY - 162 + earWig), Point(cx + 70, baseY - 145) };
    g.FillPolygon(&face, earL, 3);
    g.FillPolygon(&face, earR, 3);
    g.DrawPolygon(&out, earL, 3);
    g.DrawPolygon(&out, earR, 3);

    SolidBrush eye(Color(255, 235, 235, 235));
    int ey = baseY - 78;
    g.FillEllipse(&eye, cx - 60 + eyeShift, ey, 18, 18);
    g.FillEllipse(&eye, cx + 42 + eyeShift, ey, 18, 18);

    g.DrawLine(&whisk, cx - 55, baseY - 55, cx - 145, baseY - 70);
    g.DrawLine(&whisk, cx - 55, baseY - 45, cx - 145, baseY - 45);
    g.DrawLine(&whisk, cx - 55, baseY - 35, cx - 145, baseY - 20);

    g.DrawLine(&whisk, cx + 55, baseY - 55, cx + 145, baseY - 70);
    g.DrawLine(&whisk, cx + 55, baseY - 45, cx + 145, baseY - 45);
    g.DrawLine(&whisk, cx + 55, baseY - 35, cx + 145, baseY - 20);
}

static void DrawDoneCat(Graphics& g, int cx, int cy, int t)
{
    SolidBrush face(Color(255, 56, 56, 56));
    SolidBrush body(Color(255, 50, 50, 50));
    Pen out(Color(255, 92, 92, 92), 2.f);
    Pen whisk(C_SUB, 2.f);
    SolidBrush eye(Color(255, 235, 235, 235));
    SolidBrush blush(C_ACC);

    int sway = (int)(std::sin((double)t * 0.07) * 5.0);

    RectF bodyR((REAL)(cx - 150), (REAL)(cy + 70), 300.f, 170.f);
    GraphicsPath pb;
    RoundedPath(pb, bodyR.X, bodyR.Y, bodyR.Width, bodyR.Height, 78.f);
    g.FillPath(&body, &pb);
    g.DrawPath(&out, &pb);

    RectF headR((REAL)(cx - 140), (REAL)(cy - 120), 280.f, 220.f);
    GraphicsPath ph;
    RoundedPath(ph, headR.X, headR.Y, headR.Width, headR.Height, 74.f);
    g.FillPath(&face, &ph);
    g.DrawPath(&out, &ph);

    Point earL[3] = { Point(cx - 88, cy - 118), Point(cx - 145, cy - 175), Point(cx - 35, cy - 150) };
    Point earR[3] = { Point(cx + 88, cy - 118), Point(cx + 145, cy - 175), Point(cx + 35, cy - 150) };
    g.FillPolygon(&face, earL, 3);
    g.FillPolygon(&face, earR, 3);
    g.DrawPolygon(&out, earL, 3);
    g.DrawPolygon(&out, earR, 3);

    int eyeShift = (int)(std::sin((double)t * 0.10) * 4.0);
    g.FillEllipse(&eye, cx - 58 + eyeShift, cy - 35, 22, 22);
    g.FillEllipse(&eye, cx + 36 + eyeShift, cy - 35, 22, 22);

    g.FillEllipse(&blush, cx - 95, cy - 5, 26, 14);
    g.FillEllipse(&blush, cx + 69, cy - 5, 26, 14);

    g.DrawLine(&out, cx, cy - 6, cx, cy + 12);

    g.DrawLine(&whisk, cx - 45, cy - 10, cx - 130, cy - 25);
    g.DrawLine(&whisk, cx - 45, cy, cx - 130, cy);
    g.DrawLine(&whisk, cx - 45, cy + 10, cx - 130, cy + 25);

    g.DrawLine(&whisk, cx + 45, cy - 10, cx + 130, cy - 25);
    g.DrawLine(&whisk, cx + 45, cy, cx + 130, cy);
    g.DrawLine(&whisk, cx + 45, cy + 10, cx + 130, cy + 25);

    Pen tail(C_ACC, 6.f);
    tail.SetStartCap(LineCapRound);
    tail.SetEndCap(LineCapRound);

    int tailX = cx + 120;
    int tailY = cy + 160;

    GraphicsPath tp;
    tp.AddBezier(
        Point(tailX, tailY),
        Point(tailX + 30 + sway, tailY - 10),
        Point(tailX + 55 + sway, tailY + 35),
        Point(tailX + 78, tailY + 18)
    );
    g.DrawPath(&tail, &tp);
}

void DrawHome(Graphics& g, const RECT& rc)
{
    int w = rc.right;
    int h = rc.bottom;

    SolidBrush bg(C_BG);
    g.FillRectangle(&bg, 0, 0, w, h);

    DrawWindowAccent(g, w, h);

    DrawTextG(g, L"Welcome", 0.f, 96.f, (float)w, 44.f, 34.f, C_TEXT, true, 0);
    DrawTextG(g, L"Choose a tool", 0.f, 142.f, (float)w, 28.f, 15.f, C_SUB, false, 0);

    int bw = 180;
    int bx = (w - (bw * 3 + 24 * 2)) / 2;
    int by = 220;

    RectI b1{ bx, by, bw, 44 };
    RectI b2{ bx + bw + 24, by, bw, 44 };
    RectI b3{ bx + (bw + 24) * 2, by, bw, 44 };

    DrawButton(g, b1, L"Denoise", false, true);
    DrawButton(g, b2, L"Merge", false, true);
    DrawButton(g, b3, L"Rename", false, true);

    DrawTextG(g, L"Tip: after choosing a tool, you can drag & drop a folder/files.",
        0.f, (float)(by + 64), (float)w, 24.f, 13.f, C_SUB, false, 0);

    DrawPeekingCat(g, w, h, g_animTick);
}

void DrawPick(Graphics& g, const RECT& rc)
{
    int w = rc.right;
    int h = rc.bottom;

    SolidBrush bg(C_BG);
    g.FillRectangle(&bg, 0, 0, w, h);

    DrawToolbarCommon(g, w, true);

    RectI btnChoose{ PAD + 58, 12, 160, 32 };
    DrawButton(g, btnChoose, L"Choose folder", false, true);

    std::wstring toolName = (g_tool == Tool::Denoise) ? L"Denoise"
        : (g_tool == Tool::Merge) ? L"Merge"
        : L"Rename";
    DrawTextG(g, toolName, (float)(PAD + 230), 0.f, (float)(w - PAD - 230), (float)TOOLBAR_H, 16.f, C_TEXT, true, -1);

    int viewTop = TOOLBAR_H;
    int viewH = h - TOOLBAR_H - FOOTER_H;

    Rect clip(0, viewTop, w, viewH);
    g.SetClip(clip);

    if (g_inputs.empty())
    {
        std::wstring hint = L"Drop a folder or files here\nor use 'Choose folder'";
        DrawTextG(g, hint, 0.f, (float)viewTop, (float)w, (float)viewH, 22.f, C_TEXT, true, 0);
    }
    else
    {
        int cols = ComputeCols(w);
        int step = CELL + GAP;
        int y0 = viewTop + PAD - g_scrollY;

        int firstRow = max(0, (g_scrollY - PAD) / step);
        int rowsVisible = (viewH / step) + 3;

        int totalRows = (int)((g_inputs.size() + cols - 1) / cols);
        int lastRow = min(totalRows - 1, firstRow + rowsVisible);

        for (int row = firstRow; row <= lastRow; row++)
        {
            for (int col = 0; col < cols; col++)
            {
                size_t i = (size_t)row * (size_t)cols + (size_t)col;
                if (i >= g_inputs.size()) break;

                int x = PAD + col * step;
                int y = y0 + row * step;

                EnsureThumb(i);
                Rect r(x, y, CELL, CELL);

                if (g_thumbs[i] && g_thumbs[i]->GetLastStatus() == Ok)
                    g.DrawImage(g_thumbs[i].get(), r);
                else
                {
                    SolidBrush ph(Color(255, 18, 18, 18));
                    Pen br(Color(255, 45, 45, 45), 1.0f);
                    g.FillRectangle(&ph, r);
                    g.DrawRectangle(&br, r);
                }
            }
        }
    }

    g.ResetClip();

    bool canNext = !g_inputs.empty();
    DrawFooter(g, w, h, true, true, L"Next", !canNext);

    DrawWindowAccent(g, w, h);
    DrawTooltip(g, w, h);
}

static std::wstring FieldTextWithCaret(EditField f, const std::wstring& v)
{
    if (g_editField != f) return v;
    return g_editBuf + L"|";
}

void DrawSetup(Graphics& g, const RECT& rc)
{
    int w = rc.right;
    int h = rc.bottom;

    SolidBrush bg(C_BG);
    g.FillRectangle(&bg, 0, 0, w, h);

    DrawToolbarCommon(g, w, true);

    RectI backHome{ PAD, 12, 42, 32 };
    DrawBackArrow(g, backHome);

    std::wstring toolName =
        (g_tool == Tool::Denoise) ? L"Denoise setup" :
        (g_tool == Tool::Merge) ? L"Merge setup" :
        L"Rename setup";

    DrawTextG(g, toolName, (float)(PAD + 58), 0.f, (float)(w - PAD - 58), (float)TOOLBAR_H, 16.f, C_TEXT, true, -1);

    int y = TOOLBAR_H + 28;

    Pen br(C_BORDER, 1.0f);
    SolidBrush box(Color(255, 28, 28, 28));

    if (g_tool == Tool::Denoise)
    {
        DrawTextG(g, L"Output folder", (float)PAD, (float)y, (float)w, 24.f, 14.f, C_SUB, false, -1);

        RectI pickOut{ PAD, y + 28, 420, 38 };
        g.FillRectangle(&box, pickOut.x, pickOut.y, pickOut.w, pickOut.h);
        g.DrawRectangle(&br, pickOut.x, pickOut.y, pickOut.w, pickOut.h);
        Pen acc(C_ACC, 2.0f);
        g.DrawLine(&acc, pickOut.x, pickOut.y + pickOut.h - 1, pickOut.x + pickOut.w, pickOut.y + pickOut.h - 1);

        std::wstring outLine = g_outputFolder.empty() ? L"Choose output folder..." : EllipsizePath(g_outputFolder, 70);
        DrawTextG(g, outLine, (float)pickOut.x + 10.f, (float)pickOut.y, (float)pickOut.w - 20.f, (float)pickOut.h, 13.f,
            g_outputFolder.empty() ? C_SUB : C_TEXT, false, -1);

        int y2 = y + 92;

        DrawTextG(g, L"Result width (px)", (float)PAD, (float)y2, 200.f, 24.f, 14.f, C_SUB, false, -1);

        RectI wBox{ PAD + 210, y2 - 2, 120, 28 };
        SolidBrush wbg(Color(255, 28, 28, 28));
        g.FillRectangle(&wbg, wBox.x, wBox.y, wBox.w, wBox.h);
        g.DrawRectangle(&br, wBox.x, wBox.y, wBox.w, wBox.h);
        DrawTextG(g, std::to_wstring(g_outWidth), (float)wBox.x, (float)wBox.y, (float)wBox.w, (float)wBox.h, 14.f, C_TEXT, true, 0);

        RectI wMinus{ wBox.x + wBox.w + 10, wBox.y, 30, 28 };
        RectI wPlus{ wMinus.x + 38, wBox.y, 30, 28 };
        DrawButton(g, wMinus, L"-", false, false);
        DrawButton(g, wPlus, L"+", false, false);

        int y3 = y2 + 52;
        DrawTextG(g, L"Mode", (float)PAD, (float)y3, (float)w, 24.f, 14.f, C_SUB, false, -1);

        RectI m1{ PAD, y3 + 28, 270, 32 };
        RectI m2{ PAD, y3 + 72, 270, 32 };
        RectI m3{ PAD, y3 + 116, 270, 32 };

        RectI i1{ PAD + 292, y3 + 32, 18, 18 };
        RectI i2{ PAD + 292, y3 + 76, 18, 18 };
        RectI i3{ PAD + 292, y3 + 120, 18, 18 };

        auto drawMode = [&](const RectI& r, const wchar_t* label, DenoiseMode m)
            {
                bool active = (g_dnMode == m);
                SolidBrush bb(active ? Color(255, 36, 36, 36) : Color(255, 28, 28, 28));
                Pen bbr(active ? C_ACC : C_BORDER, active ? 2.f : 1.f);
                g.FillRectangle(&bb, r.x, r.y, r.w, r.h);
                g.DrawRectangle(&bbr, r.x, r.y, r.w, r.h);
                DrawTextG(g, label, (float)r.x + 12.f, (float)r.y, (float)r.w - 24.f, (float)r.h, 14.f, active ? C_TEXT : C_SUB, active, -1);
            };

        drawMode(m1, L"Manga", DenoiseMode::Manga);
        drawMode(m2, L"Color", DenoiseMode::Color);
        drawMode(m3, L"Balanced", DenoiseMode::Balanced);

        DrawInfoIcon(g, i1);
        DrawInfoIcon(g, i2);
        DrawInfoIcon(g, i3);

        bool canStart = !g_outputFolder.empty() && !g_inputs.empty();
        DrawFooter(g, w, h, true, true, L"Start", !canStart);
    }
    else if (g_tool == Tool::Merge)
    {
        DrawTextG(g, L"Output folder", (float)PAD, (float)y, (float)w, 24.f, 14.f, C_SUB, false, -1);

        RectI pickFolder{ PAD, y + 28, 420, 38 };
        g.FillRectangle(&box, pickFolder.x, pickFolder.y, pickFolder.w, pickFolder.h);
        g.DrawRectangle(&br, pickFolder.x, pickFolder.y, pickFolder.w, pickFolder.h);
        Pen acc(C_ACC, 2.0f);
        g.DrawLine(&acc, pickFolder.x, pickFolder.y + pickFolder.h - 1, pickFolder.x + pickFolder.w, pickFolder.y + pickFolder.h - 1);

        std::wstring outLine = g_mergeOutFolder.empty() ? L"Choose output folder..." : EllipsizePath(g_mergeOutFolder, 70);
        DrawTextG(g, outLine, (float)pickFolder.x + 10.f, (float)pickFolder.y, (float)pickFolder.w - 20.f, (float)pickFolder.h, 13.f,
            g_mergeOutFolder.empty() ? C_SUB : C_TEXT, false, -1);

        int y2 = y + 92;
        DrawTextG(g, L"File name", (float)PAD, (float)y2, 120.f, 24.f, 14.f, C_SUB, false, -1);

        RectI nBox{ PAD + 120, y2 - 2, 260, 28 };
        g.FillRectangle(&box, nBox.x, nBox.y, nBox.w, nBox.h);
        g.DrawRectangle(&br, nBox.x, nBox.y, nBox.w, nBox.h);

        std::wstring shown = (g_editField == EditField::MergeName) ? FieldTextWithCaret(EditField::MergeName, g_mergeName) : g_mergeName;
        DrawTextG(g, shown, (float)nBox.x + 10.f, (float)nBox.y, (float)nBox.w - 20.f, (float)nBox.h, 14.f, C_TEXT, false, -1);

        int fy = y2 + 44;
        DrawTextG(g, L"Format", (float)PAD, (float)fy, 120.f, 24.f, 14.f, C_SUB, false, -1);

        RectI fPng{ PAD + 120, fy - 2, 100, 28 };
        RectI fJpg{ PAD + 230, fy - 2, 110, 28 };

        auto drawFmt = [&](const RectI& r, const wchar_t* label, bool active)
            {
                SolidBrush bb(active ? Color(255, 36, 36, 36) : Color(255, 28, 28, 28));
                Pen bbr(active ? C_ACC : C_BORDER, active ? 2.f : 1.f);
                g.FillRectangle(&bb, r.x, r.y, r.w, r.h);
                g.DrawRectangle(&bbr, r.x, r.y, r.w, r.h);
                DrawTextG(g, label, (float)r.x, (float)r.y, (float)r.w, (float)r.h, 13.f, active ? C_TEXT : C_SUB, active, 0);
            };

        drawFmt(fPng, L"PNG", !g_mergeJpeg);
        drawFmt(fJpg, L"JPEG", g_mergeJpeg);

        std::wstring preview = L"Will save: " + g_mergeName + (g_mergeJpeg ? L".jpg" : L".png");
        DrawTextG(g, preview, (float)PAD, (float)(fy + 34), (float)w - PAD * 2, 22.f, 13.f, C_SUB, false, -1);

        bool canStart = !g_mergeOutFolder.empty() && !g_inputs.empty();
        DrawFooter(g, w, h, true, true, L"Start", !canStart);
    }
    else // Rename
    {
        DrawTextG(g, L"Output folder", (float)PAD, (float)y, (float)w, 24.f, 14.f, C_SUB, false, -1);

        RectI pickOut{ PAD, y + 28, 420, 38 };
        g.FillRectangle(&box, pickOut.x, pickOut.y, pickOut.w, pickOut.h);
        g.DrawRectangle(&br, pickOut.x, pickOut.y, pickOut.w, pickOut.h);
        Pen acc(C_ACC, 2.0f);
        g.DrawLine(&acc, pickOut.x, pickOut.y + pickOut.h - 1, pickOut.x + pickOut.w, pickOut.y + pickOut.h - 1);

        std::wstring outLine = g_outputFolder.empty() ? L"Choose output folder..." : EllipsizePath(g_outputFolder, 70);
        DrawTextG(g, outLine, (float)pickOut.x + 10.f, (float)pickOut.y, (float)pickOut.w - 20.f, (float)pickOut.h, 13.f,
            g_outputFolder.empty() ? C_SUB : C_TEXT, false, -1);

        int y2 = y + 92;

        DrawTextG(g, L"Prefix", (float)PAD, (float)y2, 120.f, 24.f, 14.f, C_SUB, false, -1);
        RectI pBox{ PAD + 120, y2 - 2, 260, 28 };
        g.FillRectangle(&box, pBox.x, pBox.y, pBox.w, pBox.h);
        g.DrawRectangle(&br, pBox.x, pBox.y, pBox.w, pBox.h);

        std::wstring pShown = (g_editField == EditField::RenPrefix) ? (g_editBuf + L"|") : g_renPrefix;
        DrawTextG(g, pShown, (float)pBox.x + 10.f, (float)pBox.y, (float)pBox.w - 20.f, (float)pBox.h, 14.f, C_TEXT, false, -1);

        int y3 = y2 + 40;
        DrawTextG(g, L"Suffix", (float)PAD, (float)y3, 120.f, 24.f, 14.f, C_SUB, false, -1);
        RectI sBox{ PAD + 120, y3 - 2, 260, 28 };
        g.FillRectangle(&box, sBox.x, sBox.y, sBox.w, sBox.h);
        g.DrawRectangle(&br, sBox.x, sBox.y, sBox.w, sBox.h);

        std::wstring sShown = (g_editField == EditField::RenSuffix) ? (g_editBuf + L"|") : g_renSuffix;
        DrawTextG(g, sShown, (float)sBox.x + 10.f, (float)sBox.y, (float)sBox.w - 20.f, (float)sBox.h, 14.f, C_TEXT, false, -1);

        int y4 = y3 + 46;
        DrawTextG(g, L"Start number", (float)PAD, (float)y4, 160.f, 24.f, 14.f, C_SUB, false, -1);

        RectI nBox{ PAD + 160, y4 - 2, 120, 28 };
        SolidBrush wbg(Color(255, 28, 28, 28));
        g.FillRectangle(&wbg, nBox.x, nBox.y, nBox.w, nBox.h);
        g.DrawRectangle(&br, nBox.x, nBox.y, nBox.w, nBox.h);
        DrawTextG(g, std::to_wstring(g_renStart), (float)nBox.x, (float)nBox.y, (float)nBox.w, (float)nBox.h, 14.f, C_TEXT, true, 0);

        RectI nMinus{ nBox.x + nBox.w + 10, nBox.y, 30, 28 };
        RectI nPlus{ nMinus.x + 38, nBox.y, 30, 28 };
        DrawButton(g, nMinus, L"-", false, false);
        DrawButton(g, nPlus, L"+", false, false);

        int y5 = y4 + 46;
        DrawTextG(g, L"Zero padding", (float)PAD, (float)y5, 160.f, 24.f, 14.f, C_SUB, false, -1);

        RectI zBox{ PAD + 160, y5 - 2, 120, 28 };
        g.FillRectangle(&wbg, zBox.x, zBox.y, zBox.w, zBox.h);
        g.DrawRectangle(&br, zBox.x, zBox.y, zBox.w, zBox.h);
        DrawTextG(g, std::to_wstring(g_renPad), (float)zBox.x, (float)zBox.y, (float)zBox.w, (float)zBox.h, 14.f, C_TEXT, true, 0);

        RectI zMinus{ zBox.x + zBox.w + 10, zBox.y, 30, 28 };
        RectI zPlus{ zMinus.x + 38, zBox.y, 30, 28 };
        DrawButton(g, zMinus, L"-", false, false);
        DrawButton(g, zPlus, L"+", false, false);

        std::wstring example = g_renPrefix + PadNumber(g_renStart, g_renPad) + g_renSuffix + L".ext";
        DrawTextG(g, L"Example: " + example, (float)PAD, (float)(y5 + 40), (float)w - PAD * 2, 24.f, 13.f, C_SUB, false, -1);

        bool canStart = !g_outputFolder.empty() && !g_inputs.empty();
        DrawFooter(g, w, h, true, true, L"Start", !canStart);
    }

    DrawWindowAccent(g, w, h);
    DrawTooltip(g, w, h);
}

void DrawProcessing(Graphics& g, const RECT& rc)
{
    int w = rc.right;
    int h = rc.bottom;

    SolidBrush bg(C_BG);
    g.FillRectangle(&bg, 0, 0, w, h);

    DrawToolbarCommon(g, w, true);

    std::wstring title = (g_tool == Tool::Denoise) ? L"Denoise"
        : (g_tool == Tool::Merge) ? L"Merge"
        : L"Rename";
    DrawTextG(g, title, (float)(PAD + 58), 0.f, (float)(w - PAD - 58), (float)TOOLBAR_H, 16.f, C_TEXT, true, -1);

    DrawTextG(g, L"Processing...", 0.f, (float)(TOOLBAR_H + 70), (float)w, 34.f, 24.f, C_TEXT, true, 0);

    int barX = PAD;
    int barY = TOOLBAR_H + 140;
    int barW = w - PAD * 2;
    int barH = 18;

    SolidBrush barBg(Color(255, 40, 40, 40));
    g.FillRectangle(&barBg, barX, barY, barW, barH);

    int total = max(1, g_total);
    int done = ClampI(g_processed, 0, total);
    float k = (float)done / (float)total;
    int fillW = (int)(barW * k);

    SolidBrush barFill(C_ACC);
    g.FillRectangle(&barFill, barX, barY, fillW, barH);

    Pen br(C_BORDER, 1.0f);
    g.DrawRectangle(&br, barX, barY, barW, barH);

    std::wstring line = L"Done: " + std::to_wstring(done) + L" / " + std::to_wstring(g_total);
    DrawTextG(g, line, 0.f, (float)(barY + 28), (float)w, 24.f, 14.f, C_SUB, false, 0);

    DrawFooter(g, w, h, true, false, L"", true);
    DrawWindowAccent(g, w, h);
}

void DrawDone(Graphics& g, const RECT& rc)
{
    int w = rc.right;
    int h = rc.bottom;

    SolidBrush bg(C_BG);
    g.FillRectangle(&bg, 0, 0, w, h);

    DrawToolbarCommon(g, w, true);

    DrawTextG(g, L"Completed", 0.f, (float)(TOOLBAR_H + 40), (float)w, 44.f, 30.f, C_TEXT, true, 0);

    std::wstring timeLine = L"Total time: " + FormatDuration(g_elapsedMs);
    DrawTextG(g, timeLine, 0.f, (float)(TOOLBAR_H + 86), (float)w, 26.f, 14.f, C_SUB, false, 0);

    int cx = w / 2;
    int cy = TOOLBAR_H + 290;
    DrawDoneCat(g, cx, cy, g_animTick);

    RectI again{ w / 2 - 170, h - FOOTER_H - 70, 160, 40 };
    RectI home{ w / 2 + 10,  h - FOOTER_H - 70, 160, 40 };
    DrawButton(g, again, L"Process more", false, true);
    DrawButton(g, home, L"Home", false, true);

    DrawFooter(g, w, h, true, false, L"", true);
    DrawWindowAccent(g, w, h);
}
