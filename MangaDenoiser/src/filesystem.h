#pragma once
#include "app_state.h"

bool IsImageFile(const std::wstring& p);
void CollectFromFolder(const std::wstring& folder, std::vector<std::wstring>& out, bool allFiles);
std::wstring PickFolderDialog(HWND hWnd, const wchar_t* title);

void LoadInputsFromFolder(const std::wstring& folder);
void LoadInputsFromDrop(HDROP hDrop);
