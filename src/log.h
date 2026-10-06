#pragma once
#include <cstdio>
#include <cstdarg>
#include <windows.h>
namespace vrlog {
inline void write(const char* fmt, ...) {
    static CRITICAL_SECTION cs; static bool init = false;
    if (!init) { InitializeCriticalSection(&cs); init = true; }
    EnterCriticalSection(&cs);
    FILE* f = fopen("GTA5VR.log", "a");   // the game's working folder = {game}
    if (f) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
        va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
        fputc('\n', f); fclose(f);
    }
    LeaveCriticalSection(&cs);
}
}
