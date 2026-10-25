#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace
{
    std::wstring g_path;
    std::mutex g_mutex;
}

void LogInit(const std::wstring& dir)
{
    g_path = dir + L"\\DevourTimer.log";
    FILE* f = _wfopen(g_path.c_str(), L"wb"); // 起動のたびに作り直す
    if (f) fclose(f);
}

void Log(const char* fmt, ...)
{
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_path.empty()) return;
    FILE* f = _wfopen(g_path.c_str(), L"ab");
    if (!f) return;
    fprintf(f, "[%02d:%02d:%02d.%03d] %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, body);
    fclose(f);
}
