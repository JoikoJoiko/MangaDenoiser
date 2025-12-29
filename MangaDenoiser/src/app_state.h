#pragma once
#define NOMINMAX
#include <windows.h>
#include <gdiplus.h>

#include <string>
#include <vector>
#include <memory>
#include <algorithm>

using namespace Gdiplus;
using std::min;
using std::max;

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Comdlg32.lib")

extern const wchar_t WINDOW_CLASS[];
extern const wchar_t WINDOW_TITLE[];

extern const int TOOLBAR_H;
extern const int FOOTER_H;
extern const int PAD;
extern const int CELL;
extern const int GAP;

inline constexpr UINT_PTR TIMER_UI = 1;
inline constexpr UINT_PTR TIMER_ANIM = 2;

inline constexpr UINT WM_APP_PROGRESS = WM_APP + 1;
inline constexpr UINT WM_APP_DONE = WM_APP + 2;

struct RectI { int x, y, w, h; };
bool PtIn(const RectI& r, int px, int py);
int ClampI(int v, int a, int b);
int iabs(int v);

extern Color C_BG;
extern Color C_PANEL;
extern Color C_BTN;
extern Color C_BTN_DIS;
extern Color C_BORDER;
extern Color C_TEXT;
extern Color C_SUB;
extern Color C_ACC;

enum class View { Home, Pick, Setup, Processing, Done };
enum class Tool { None, Denoise, Merge, Rename };
enum class DenoiseMode { Manga, Color, Balanced };
enum class EditField { None, MergeName, RenPrefix, RenSuffix };

extern ULONG_PTR g_gdiplusToken;

extern View g_view;
extern Tool g_tool;

extern std::vector<std::wstring> g_inputs;
extern std::vector<std::unique_ptr<Bitmap>> g_thumbs;

extern std::wstring g_inputFolder;
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
extern int g_ttId;

extern EditField g_editField;
extern std::wstring g_editBuf;

extern HANDLE g_worker;
extern HWND g_hWndMain;

void SetStatus(const std::wstring& s);
std::wstring FormatDuration(long long ms);

int ComputeCols(int clientW);
int ComputeContentHeight(int cols, int count);
void ResetScroll();
void ComputeScrollMax(const RECT& rc);

std::wstring GetExtLower(const std::wstring& path);
std::wstring GetFileNameOnly(const std::wstring& path);
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
std::wstring EllipsizePath(const std::wstring& s, int maxChars);
std::wstring PadNumber(int v, int width);

void ClearAllStateToHome();
void ClearToolStateKeepTool();
