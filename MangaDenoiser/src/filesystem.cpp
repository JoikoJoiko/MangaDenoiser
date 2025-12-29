#include "framework.h"
#include "filesystem.h"
#include <shellapi.h>
#include <shlobj.h>

bool IsImageFile(const std::wstring& p)
{
    std::wstring ext = GetExtLower(p);
    return ext == L"png" || ext == L"jpg" || ext == L"jpeg" || ext == L"bmp" || ext == L"webp";
}

void CollectFromFolder(const std::wstring& folder, std::vector<std::wstring>& out, bool allFiles)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((folder + L"\\*.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring f = folder + L"\\" + fd.cFileName;
        if (allFiles) out.push_back(f);
        else if (IsImageFile(f)) out.push_back(f);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

std::wstring PickFolderDialog(HWND hWnd, const wchar_t* title)
{
    BROWSEINFOW bi{};
    bi.hwndOwner = hWnd;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return L"";

    wchar_t path[MAX_PATH]{};
    if (!SHGetPathFromIDListW(pidl, path))
    {
        CoTaskMemFree(pidl);
        return L"";
    }
    CoTaskMemFree(pidl);
    return path;
}

void LoadInputsFromFolder(const std::wstring& folder)
{
    g_inputs.clear();
    g_thumbs.clear();

    bool allFiles = (g_tool == Tool::Rename);
    CollectFromFolder(folder, g_inputs, allFiles);

    std::sort(g_inputs.begin(), g_inputs.end());

    g_thumbs.resize(g_inputs.size());
    for (auto& t : g_thumbs) t.reset();

    g_inputFolder = folder;
    ResetScroll();

    SetStatus(g_inputs.empty() ? L"No files found" : L"Loaded");
}

void LoadInputsFromDrop(HDROP hDrop)
{
    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    wchar_t path[MAX_PATH]{};

    std::vector<std::wstring> collected;
    bool allFiles = (g_tool == Tool::Rename);

    for (UINT i = 0; i < count; i++)
    {
        if (!DragQueryFileW(hDrop, i, path, MAX_PATH)) continue;
        std::wstring p = path;
        DWORD attr = GetFileAttributesW(p.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;

        if (attr & FILE_ATTRIBUTE_DIRECTORY)
        {
            g_inputFolder = p;
            CollectFromFolder(p, collected, allFiles);
        }
        else
        {
            if (allFiles) collected.push_back(p);
            else if (IsImageFile(p)) collected.push_back(p);
        }
    }

    g_inputs = std::move(collected);
    std::sort(g_inputs.begin(), g_inputs.end());

    g_thumbs.clear();
    g_thumbs.resize(g_inputs.size());
    for (auto& t : g_thumbs) t.reset();

    ResetScroll();
    SetStatus(g_inputs.empty() ? L"No files found" : L"Loaded");
}
