#pragma once

#include "framework.h"   
#include "app_state.h"   

void CommitEdit();
void CancelEdit();
void StartEdit(EditField f, const std::wstring& initial);

void HitHomeClick(HWND hWnd, int mx, int my, const RECT& rc);
void HandlePickClick(HWND hWnd, int mx, int my, const RECT& rc);
void HandleSetupClick(HWND hWnd, int mx, int my, const RECT& rc);
void HandleDoneClick(HWND hWnd, int mx, int my, const RECT& rc);

bool IsAllowedChar(wchar_t c);
void HandleCharInput(HWND hWnd, wchar_t ch);
