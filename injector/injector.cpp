// DevourTimer.exe
// ダブルクリックするだけで:
//   1. DEVOUR が起動していなければ Steam 経由で起動する
//   2. ゲームの準備ができるまで待つ
//   3. 同じフォルダの DevourTimerNative.dll を DEVOUR.exe に読み込ませる
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
    const wchar_t* kGameExe = L"DEVOUR.exe";
    const wchar_t* kDllName = L"DevourTimerNative.dll";
    const wchar_t* kSteamUrl = L"steam://rungameid/1274570";

    DWORD FindProcess(const wchar_t* exe)
    {
        DWORD pid = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(snap, &pe))
        {
            do
            {
                if (_wcsicmp(pe.szExeFile, exe) == 0) { pid = pe.th32ProcessID; break; }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return pid;
    }

    bool HasModule(DWORD pid, const wchar_t* name)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) return false;
        MODULEENTRY32W me = { sizeof(me) };
        bool found = false;
        if (Module32FirstW(snap, &me))
        {
            do
            {
                if (_wcsicmp(me.szModule, name) == 0) { found = true; break; }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
        return found;
    }

    bool Inject(DWORD pid, const std::wstring& dll)
    {
        HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
        if (!proc) { printf("ゲームのプロセスを開けませんでした (エラー %lu)\n", GetLastError()); return false; }

        size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
        void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        bool ok = remote && WriteProcessMemory(proc, remote, dll.c_str(), bytes, nullptr);

        if (ok)
        {
            // kernel32 はどのプロセスでも同じアドレスに載るので、自分の LoadLibraryW のアドレスがそのまま使える
            auto loadLibrary = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
            HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLibrary, remote, 0, nullptr);
            ok = th != nullptr;
            if (th)
            {
                WaitForSingleObject(th, 10000);
                DWORD code = 0;
                GetExitCodeThread(th, &code);
                ok = code != 0; // LoadLibraryW の戻り値 (下位 32bit)。0 なら読み込み失敗
                CloseHandle(th);
            }
        }
        if (remote) VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return ok;
    }

    std::wstring ExeDir()
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring s = path;
        return s.substr(0, s.find_last_of(L"\\/"));
    }

    void Finish(int code)
    {
        printf("\n%s", code == 0 ? "3 秒後に閉じます。\n" : "Enter キーで閉じます。\n");
        if (code == 0) Sleep(3000);
        else getchar();
        exit(code); // ExitProcess だと printf のバッファが捨てられる
    }
}

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    printf("DEVOUR Speedrun Timer\n\n");

    std::wstring dll = ExeDir() + L"\\" + kDllName;
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        printf("DevourTimerNative.dll が見つかりません。DevourTimer.exe と同じフォルダに置いてください。\n");
        Finish(1);
    }

    DWORD pid = FindProcess(kGameExe);
    if (!pid)
    {
        printf("DEVOUR を Steam で起動します...\n");
        ShellExecuteW(nullptr, L"open", kSteamUrl, nullptr, nullptr, SW_SHOWNORMAL);
        for (int i = 0; i < 180 && !pid; i++)
        {
            Sleep(1000);
            pid = FindProcess(kGameExe);
        }
        if (!pid) { printf("3 分待っても DEVOUR が起動しませんでした。\n"); Finish(1); }
    }
    printf("DEVOUR を見つけました (PID %lu)\n", pid);

    if (HasModule(pid, kDllName))
    {
        printf("タイマーはもう読み込まれています。\n");
        Finish(0);
    }

    // GameAssembly.dll (ゲーム本体のコード) が載るまで待つ。
    // DLL 側でもゲームウィンドウが出るまで待ってから動き出すので、ここは載っていれば十分。
    printf("ゲームの準備を待っています...\n");
    for (int i = 0; i < 180 && !HasModule(pid, L"GameAssembly.dll"); i++) Sleep(1000);
    if (!HasModule(pid, L"GameAssembly.dll")) { printf("ゲームの準備が終わりませんでした。\n"); Finish(1); }

    if (!Inject(pid, dll))
    {
        printf("読み込みに失敗しました。ゲームを管理者として起動している場合は、こちらも管理者として実行してください。\n");
        Finish(1);
    }

    printf("読み込み完了。数秒後に画面左上にタイマーが出ます。\n");
    printf("  F5 = リセット / F6 = デバッグ表示 / F7・F8 = エレベーター・トラックの範囲 / F9 = 表示切替\n");
    Finish(0);
}
