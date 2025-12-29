#pragma once
#include "app_state.h"

void DrawPeekingCat(Graphics& g, int w, int h, int t);
void DrawDoneCat(Graphics& g, int cx, int cy, int t);

void DrawHome(Graphics& g, const RECT& rc);
void DrawPick(Graphics& g, const RECT& rc);
void DrawSetup(Graphics& g, const RECT& rc);
void DrawProcessing(Graphics& g, const RECT& rc);
void DrawDone(Graphics& g, const RECT& rc);
