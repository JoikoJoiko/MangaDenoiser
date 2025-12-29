#pragma once

#include "framework.h"   
#include "app_state.h"   

void StartEdit(EditField f, const std::wstring& initial);
void CancelEdit();
void CommitEdit();
void HandleCharInput(HWND hWnd, wchar_t ch);

void HitHomeClick(HWND hWnd, int mx, int my, const RECT& rc);
void HandlePickClick(HWND hWnd, int mx, int my, const RECT& rc);
void HandleSetupClick(HWND hWnd, int mx, int my, const RECT& rc);
void HandleDoneClick(HWND hWnd, int mx, int my, const RECT& rc);

void HandleSetupMouseMove(HWND hWnd, int mx, int my, const RECT& rc);
void HandleSetupLButtonUp(HWND hWnd, int mx, int my, const RECT& rc);
void HandleSetupRButtonDown(HWND hWnd, int mx, int my, const RECT& rc);
void HandleSetupMouseWheel(HWND hWnd, short wheelDelta, int mx, int my, const RECT& rc);
