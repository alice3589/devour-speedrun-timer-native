// DevourTimer.exe (インジェクター) から DEVOUR.exe に読み込まれる本体。
// DllMain の中では重い処理ができないので、別スレッドを立ててそこで準備する。
#include <windows.h>
#include <MinHook.h>

#include "il2cpp.h"
#include "log.h"
#include "overlay.h"
#include "timer.h"

namespace
{
    HMODULE g_self;

    std::wstring ModuleDir()
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(g_self, path, MAX_PATH);
        std::wstring s = path;
        return s.substr(0, s.find_last_of(L"\\/"));
    }

    BOOL CALLBACK FindUnityWindow(HWND hwnd, LPARAM found)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId()) return TRUE;
        wchar_t cls[64];
        GetClassNameW(hwnd, cls, 64);
        if (wcscmp(cls, L"UnityWndClass") != 0) return TRUE;
        *reinterpret_cast<bool*>(found) = true;
        return FALSE;
    }

    DWORD WINAPI Setup(LPVOID)
    {
        LogInit(ModuleDir());
        Log("DevourTimer 読み込み");

        // il2cpp の初期化はゲームウィンドウが出る前に終わる。
        // 早すぎるタイミングで il2cpp を触ると落ちるので、ウィンドウが出るまで待つ。
        for (;;)
        {
            bool found = false;
            if (GetModuleHandleW(L"GameAssembly.dll")) EnumWindows(FindUnityWindow, (LPARAM)&found);
            if (found) break;
            Sleep(500);
        }
        Sleep(3000);

        if (!il2cpp::Init()) { Log("il2cpp API の解決に失敗。中止します"); return 0; }
        if (MH_Initialize() != MH_OK) { Log("MinHook の初期化に失敗。中止します"); return 0; }

        if (!TimerInit(ModuleDir())) { Log("ゲーム側のフックに失敗。中止します"); return 0; }
        if (!OverlayInit()) { Log("描画フックに失敗。計測はしますが表示されません"); return 0; }

        Log("準備完了。F5=リセット F6=デバッグ F7/F8=範囲の設定 F9=表示切替");
        return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = module;
        DisableThreadLibraryCalls(module);
        // 二重注入されても 2 回目は何もしない (ローダーが同じモジュールとして扱うので通常は起きない)
        CloseHandle(CreateThread(nullptr, 0, Setup, nullptr, 0, nullptr));
    }
    return TRUE;
}
