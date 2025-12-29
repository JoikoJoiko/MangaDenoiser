#include "framework.h"
#include "app_state.h"
#include <cwctype>
#include <cmath>

const wchar_t WINDOW_CLASS[] = L"MangaDenoiserWindow";
const wchar_t WINDOW_TITLE[] = L"Manga Tools";

const int TOOLBAR_H = 56;
const int FOOTER_H = 44;

const int PAD = 16;
const int CELL = 140;
const int GAP = 16;

const UINT_PTR TIMER_UI = 1;
const UINT_PTR TIMER_ANIM = 2;

bool PtIn(const RectI& r, int px, int py) { return px >= r.x && px < (r.x + r.w) && py >= r.y && py < (r.y + r.h); }
int ClampI(int v, int a, int b) { return (v < a) ? a : (v > b) ? b : v; }
int iabs(int v) { return v < 0 ? -v : v; }

Color C_BG(255, 24, 24, 24);
Color C_PANEL(255, 32, 32, 32);
Color C_BTN(255, 55, 55, 55);
Color C_BTN_DIS(255, 40, 40, 40);
Color C_BORDER(255, 92, 92, 92);
Color C_TEXT(255, 225, 225, 225);
Color C_SUB(255, 175, 175, 175);
Color C_ACC(255, 255, 120, 205);

ULONG_PTR g_gdiplusToken = 0;

View g_view = View::Home;
Tool g_tool = Tool::None;

std::vector<std::wstring> g_inputs;
std::vector<std::unique_ptr<Bitmap>> g_thumbs;

std::wstring g_inputFolder;
std::wstring g_outputFolder;

std::wstring g_status = L"Ready";

int g_scrollY = 0;
int g_scrollTarget = 0;
int g_scrollMax = 0;

int g_animTick = 0;

bool g_processing = false;
int g_processed = 0;
int g_total = 0;
long long g_elapsedMs = 0;

int g_outWidth = 1600;
DenoiseMode g_dnMode = DenoiseMode::Manga;

std::wstring g_mergeOutFolder;
bool g_mergeJpeg = false;
std::wstring g_mergeName = L"merged";

std::wstring g_renPrefix = L"image_";
std::wstring g_renSuffix = L"";
int g_renStart = 1;
int g_renPad = 3;

bool g_ttShow = false;
std::wstring g_ttText;
POINT g_ttPos{ 0,0 };
int g_ttId = -1;

EditField g_editField = EditField::None;
std::wstring g_editBuf;

HANDLE g_worker = nullptr;
HWND g_hWndMain = nullptr;

std::unique_ptr<Bitmap> g_cropBmp;
std::wstring g_cropSrcPath;

int g_cropScrollY = 0;
int g_cropScrollMax = 0;

std::vector<int> g_cropGuides;
bool g_cropDragging = false;
int  g_cropDragGuideIndex = -1;
float g_cropZoom = 1.0f;

bool g_cropJpeg = false;
int  g_cropJpegQuality = 92;



std::wstring g_cropPrefix = L"crop_";
int g_cropPad = 3;

static void UniqueSorted(std::vector<int>& v)
{
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

void CropGetSortedGuides(int imgH, std::vector<int>& outGuides)
{
    outGuides.clear();
    outGuides.reserve(g_cropGuides.size());
    for (int y : g_cropGuides)
    {
        // Internal guides only; 0 and imgH are implicit bounds.
        if (y <= 0 || y >= imgH) continue;
        outGuides.push_back(ClampI(y, 1, max(1, imgH - 1)));
    }
    UniqueSorted(outGuides);
}

int CropGetSegmentCount(int imgH)
{
    if (imgH <= 0) return 0;
    std::vector<int> g;
    CropGetSortedGuides(imgH, g);
    return (int)g.size() + 1;
}

void SetStatus(const std::wstring& s) { g_status = s; }

std::wstring FormatDuration(long long ms)
{
    if (ms < 0) ms = 0;
    long long sec = ms / 1000;
    if (sec < 60) return std::to_wstring((int)sec) + L" s";
    long long m = sec / 60;
    long long s = sec % 60;
    std::wstring out = std::to_wstring((int)m) + L" min ";
    if (s < 10) out += L"0";
    out += std::to_wstring((int)s) + L" s";
    return out;
}

int ComputeCols(int clientW)
{
    int usable = clientW - PAD * 2;
    int step = CELL + GAP;
    return max(1, usable / step);
}
int ComputeContentHeight(int cols, int count)
{
    if (count <= 0) return PAD * 2;
    int rows = (int)((count + cols - 1) / cols);
    int h = PAD * 2 + rows * (CELL + GAP) - GAP;
    return max(h, PAD * 2);
}
void ResetScroll() { g_scrollY = g_scrollTarget = g_scrollMax = 0; }

void ComputeScrollMax(const RECT& rc)
{
    int viewTop = TOOLBAR_H;
    int viewBottom = rc.bottom - FOOTER_H;
    int viewH = max(0, viewBottom - viewTop);

    int cols = ComputeCols(rc.right);
    int contentH = ComputeContentHeight(cols, (int)g_inputs.size());
    g_scrollMax = max(0, contentH - viewH);

    g_scrollTarget = ClampI(g_scrollTarget, 0, g_scrollMax);
    g_scrollY = ClampI(g_scrollY, 0, g_scrollMax);
}

std::wstring GetFileNameOnly(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return path;
    return path.substr(p + 1);
}
std::wstring GetExtLower(const std::wstring& path)
{
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return L"";
    std::wstring e = path.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(), ::towlower);
    return e;
}
std::wstring JoinPath(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}
std::wstring EllipsizePath(const std::wstring& s, int maxChars)
{
    if ((int)s.size() <= maxChars) return s;
    if (maxChars < 10) return s.substr(0, maxChars);
    int keepL = maxChars / 2 - 2;
    int keepR = maxChars - keepL - 3;
    return s.substr(0, keepL) + L"..." + s.substr((int)s.size() - keepR);
}
std::wstring PadNumber(int v, int width)
{
    std::wstring s = std::to_wstring(v);
    while ((int)s.size() < width) s = L"0" + s;
    return s;
}

static void ResetCropState()
{
    g_cropBmp.reset();
    g_cropSrcPath.clear();
    g_cropScrollY = 0;
    g_cropScrollMax = 0;
    g_cropGuides.clear();
    g_cropDragging = false;
    g_cropDragGuideIndex = -1;
    g_cropZoom = 1.0f;
    g_cropJpeg = false;
    g_cropJpegQuality = 92;
    g_cropPrefix = L"crop_";
    g_cropPad = 3;
}

void ClearAllStateToHome()
{
    g_view = View::Home;
    g_tool = Tool::None;

    g_inputs.clear();
    g_thumbs.clear();

    g_inputFolder.clear();
    g_outputFolder.clear();

    g_status = L"Ready";
    g_processing = false;
    g_processed = 0;
    g_total = 0;
    g_elapsedMs = 0;

    g_editField = EditField::None;
    g_editBuf.clear();

    ResetScroll();
    ResetCropState();
}
void ClearToolStateKeepTool()
{
    g_inputs.clear();
    g_thumbs.clear();
    g_inputFolder.clear();
    g_outputFolder.clear();

    g_status = L"Ready";
    g_processing = false;
    g_processed = 0;
    g_total = 0;
    g_elapsedMs = 0;

    g_editField = EditField::None;
    g_editBuf.clear();

    ResetScroll();
    ResetCropState();
}
