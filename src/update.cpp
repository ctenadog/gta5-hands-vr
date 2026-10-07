// Auto-update (0.6.3). GTA5VR.ini [vr] auto_update=0 turns it off.
//  1. download releases/latest/version.txt (written by CI), compare with GTA5VR_VERSION
//  2. if newer: download GTA5VR-<ver>.zip to %TEMP%\GTA5VR-update
//  3. write apply.ps1 there and start it hidden (parent explorer.exe, so Steam closing the game does not kill it):
//     it waits until this GTA5.exe exits, unpacks the zip and copies GTA5VR.asi / GTA5VR_Host.exe / openxr_loader.dll
//     into the game folder. Result goes to GTA5VR_update.log in the game folder.
#include <windows.h>
#include <urlmon.h>
#include <wininet.h>
#include <string>
#include <vector>
#include <atomic>
#include <cstdio>
#include <cstring>
#include "update.h"
#include "version.h"
#include "log.h"

namespace {
const wchar_t* kBase = L"https://github.com/ctenadog/gta5-hands-vr/releases/download/latest/";
std::atomic<int> g_event{0}; char g_newVer[32] = {0};

std::wstring gameDirW() { wchar_t m[MAX_PATH]; GetModuleFileNameW(nullptr, m, MAX_PATH); std::wstring s = m; return s.substr(0, s.find_last_of(L"\\/") + 1); }
int cmpVer(const char* a, const char* b) {
    int x[3] = {0, 0, 0}, y[3] = {0, 0, 0};
    sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]); sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; ++i) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}
bool download(const std::wstring& url, const std::wstring& file) {
    std::wstring u = url + L"?t=" + std::to_wstring(GetTickCount64());   // no cached copy
    DeleteFileW(file.c_str());
    HRESULT hr = URLDownloadToFileW(nullptr, u.c_str(), file.c_str(), 0, nullptr);
    if (FAILED(hr)) { vrlog::write("update: download failed (0x%08x)", (unsigned)hr); return false; }
    return true;
}
std::wstring psq(const std::wstring& s) { std::wstring r = L"'"; for (wchar_t c : s) { if (c == L'\'') r += L"''"; else r += c; } return r + L"'"; }
bool launchHidden(std::wstring cmd) {
    STARTUPINFOEXW si{}; si.StartupInfo.cb = sizeof(si); PROCESS_INFORMATION pi{};
    HANDLE shell = nullptr; DWORD shellPid = 0; HWND sw = GetShellWindow();
    if (sw) GetWindowThreadProcessId(sw, &shellPid);
    if (shellPid) shell = OpenProcess(PROCESS_CREATE_PROCESS, FALSE, shellPid);
    SIZE_T sz = 0; std::vector<char> attr; bool useParent = false;
    if (shell) {
        InitializeProcThreadAttributeList(nullptr, 1, 0, &sz); attr.resize(sz);
        si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attr.data();
        useParent = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &sz) &&
                    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &shell, sizeof(shell), nullptr, nullptr);
    }
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | (useParent ? EXTENDED_STARTUPINFO_PRESENT : 0),
                             nullptr, nullptr, useParent ? &si.StartupInfo : (STARTUPINFOW*)&si, &pi);
    if (!ok && useParent) { STARTUPINFOW s2{}; s2.cb = sizeof(s2); ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &s2, &pi);
                              if (!ok) ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &s2, &pi); useParent = false; }
    if (useParent) DeleteProcThreadAttributeList(si.lpAttributeList);
    if (shell) CloseHandle(shell);
    if (!ok) { vrlog::write("update: could not start the installer (%lu)", GetLastError()); return false; }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

DWORD WINAPI run(LPVOID) {
    Sleep(20000);   // let the game load first; never compete with startup
    std::wstring game = gameDirW(), ini = game + L"GTA5VR.ini";
    if (GetPrivateProfileIntW(L"vr", L"auto_update", 1, ini.c_str()) == 0) { vrlog::write("update: auto_update=0 - not checking"); return 0; }
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"GTA5VR-update\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring vf = dir + L"version.txt";
    if (!download(std::wstring(kBase) + L"version.txt", vf)) return 0;
    char ver[32] = {0};
    if (FILE* f = _wfopen(vf.c_str(), L"rb")) { size_t n = fread(ver, 1, sizeof(ver) - 1, f); ver[n] = 0; fclose(f); }
    for (char* p = ver; *p; ++p) if (*p == '\r' || *p == '\n' || *p == ' ') { *p = 0; break; }
    int a, b, c;
    if (sscanf(ver, "%d.%d.%d", &a, &b, &c) != 3) { vrlog::write("update: version.txt unreadable"); return 0; }
    if (cmpVer(ver, GTA5VR_VERSION) <= 0) { vrlog::write("update: up to date (%s, latest %s)", GTA5VR_VERSION, ver); return 0; }
    vrlog::write("update: new version %s (installed %s) - downloading", ver, GTA5VR_VERSION);
    std::wstring wver(ver, ver + strlen(ver));
    std::wstring zip = dir + L"GTA5VR-" + wver + L".zip";
    if (!download(std::wstring(kBase) + L"GTA5VR-" + wver + L".zip", zip)) return 0;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(zip.c_str(), GetFileExInfoStandard, &fa) || fa.nFileSizeLow < 200000) { vrlog::write("update: downloaded zip too small - skipped"); return 0; }
    std::wstring ps = dir + L"apply.ps1";
    std::wstring s =
        L"$ErrorActionPreference = 'Continue'\r\n"
        L"$game = " + psq(game) + L"; $zip = " + psq(zip) + L"; $x = " + psq(dir + L"x") + L"\r\n"
        L"$log = Join-Path $game 'GTA5VR_update.log'\r\n"
        L"Wait-Process -Id " + std::to_wstring(GetCurrentProcessId()) + L" -ErrorAction SilentlyContinue\r\n"
        L"Start-Sleep -Seconds 3\r\n"
        L"Get-Process GTA5VR_Host -ErrorAction SilentlyContinue | Stop-Process -Force\r\n"
        L"Remove-Item $x -Recurse -Force -ErrorAction SilentlyContinue\r\n"
        L"try { Expand-Archive $zip $x -Force } catch { Add-Content $log \"$(Get-Date) unpack failed: $_\"; exit 1 }\r\n"
        L"$ok = $true\r\n"
        L"foreach ($f in 'GTA5VR.asi','GTA5VR_Host.exe','openxr_loader.dll') {\r\n"
        L"  $src = Join-Path $x $f; if (-not (Test-Path $src)) { continue }\r\n"
        L"  $done = $false\r\n"
        L"  for ($i = 0; $i -lt 30 -and -not $done; $i++) { try { Copy-Item $src $game -Force -ErrorAction Stop; $done = $true } catch { Start-Sleep -Seconds 2 } }\r\n"
        L"  if (-not $done) { $ok = $false; Add-Content $log \"$(Get-Date) could not replace $f\" }\r\n"
        L"}\r\n"
        L"if ($ok) { Add-Content $log \"$(Get-Date) updated to " + wver + L"\"; Remove-Item $zip -Force -ErrorAction SilentlyContinue }\r\n"
        L"Remove-Item $x -Recurse -Force -ErrorAction SilentlyContinue\r\n";
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string u8(n, 0); WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u8[0], n, nullptr, nullptr);
        FILE* f = _wfopen(ps.c_str(), L"wb"); if (!f) { vrlog::write("update: cannot write apply.ps1"); return 0; }
        fwrite("\xEF\xBB\xBF", 1, 3, f); fwrite(u8.data(), 1, u8.size(), f); fclose(f);
    }
    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File \"" + ps + L"\"";
    if (!launchHidden(cmd)) return 0;
    strncpy(g_newVer, ver, sizeof(g_newVer) - 1);
    g_event = 1;
    vrlog::write("update: %s downloaded - it will be installed automatically when the game closes", ver);
    return 0;
}
}

namespace update {
void start() { CreateThread(nullptr, 0, run, nullptr, 0, nullptr); }
int takeEvent(char* ver, int n) {
    int e = g_event.exchange(0);
    if (e) { strncpy(ver, g_newVer, n - 1); ver[n - 1] = 0; }
    return e;
}
}
