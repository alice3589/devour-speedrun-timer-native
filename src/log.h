#pragma once
#include <string>

// DLL と同じフォルダの DevourTimer.log に追記する (UTF-8)。
// どの段階で失敗したかは、ゲーム内に何も出ないときこのファイルで分かる。
void LogInit(const std::wstring& dir);
void Log(const char* fmt, ...);
