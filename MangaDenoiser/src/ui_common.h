#pragma once
#include "app_state.h"

void DrawTextG(Graphics& g, const std::wstring& text, float x, float y, float w, float h, float size, Color color, bool bold, int alignH);

void RoundedPath(GraphicsPath& path, float x, float y, float w, float h, float r);
void FillRoundRect(Graphics& g, const RectF& r, float radius, Brush& fill, Pen* border);

void DrawButton(Graphics& g, const RectI& r, const wchar_t* text, bool disabled, bool accentLine = true);
void DrawBackArrow(Graphics& g, const RectI& r);
void DrawInfoIcon(Graphics& g, const RectI& r);
void DrawWindowAccent(Graphics& g, int w, int h);

void DrawToolbarCommon(Graphics& g, int w, bool showBackToHome);
void DrawFooter(Graphics& g, int w, int h, bool showBack, bool showNext, const std::wstring& nextText, bool nextDisabled);
