// speedrun.com のルールどおりに計測する。
//   開始 : 通常マップ     … 誰か1人が最初に動いた瞬間
//          Asylum         … カウントダウン後、誰か1人がエレベーターから出た瞬間
//          Slaughterhouse … 同じく、誰か1人がトラックから出た瞬間
//   終了 : 10 個目の儀式のあと「10/10」表示がフェードインし始めた最初のフレーム
//
// Unity の API はメインスレッドからしか呼べないので、判定はすべてゲームのメソッドをフックした中で行う。
//   GameUI.Update                … 毎フレーム。プレイヤー位置を見てスタート判定
//   GameUI.<ShowGoatCount>.MoveNext … 「N/10」表示をフェードさせるコルーチン。進めた直後に alpha を見てゴール判定
//   GameUI.OnDisable             … 試合シーンを抜けた
#include "timer.h"
#include "il2cpp.h"
#include "log.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <mutex>

namespace
{
    // ---------------------------------------------------------------
    // 設定 (DevourTimer.ini)
    // ---------------------------------------------------------------
    struct Config
    {
        bool showTimer = true;
        float posX = 20, posY = 20;
        int fontSize = 36;
        float moveThreshold = 0.1f;
        float teleportDistance = 3.0f;
        float defaultExitRadius = 2.5f;
        std::wstring elevatorZone; // "中心X,中心Z,半径"
        std::wstring truckZone;
    };

    std::wstring g_dir;
    std::wstring g_ini;
    Config g_cfg;

    float IniFloat(const wchar_t* key, float def)
    {
        wchar_t buf[64];
        wchar_t defs[64];
        swprintf(defs, 64, L"%g", def);
        GetPrivateProfileStringW(L"Timer", key, defs, buf, 64, g_ini.c_str());
        return (float)_wtof(buf);
    }

    std::wstring IniString(const wchar_t* key)
    {
        wchar_t buf[256];
        GetPrivateProfileStringW(L"Timer", key, L"", buf, 256, g_ini.c_str());
        return buf;
    }

    void IniWrite(const wchar_t* key, const std::wstring& value)
    {
        WritePrivateProfileStringW(L"Timer", key, value.c_str(), g_ini.c_str());
    }

    void IniWriteFloat(const wchar_t* key, float v)
    {
        wchar_t buf[64];
        swprintf(buf, 64, L"%g", v);
        IniWrite(key, buf);
    }

    void LoadConfig()
    {
        g_cfg.showTimer = IniFloat(L"ShowTimer", 1) != 0;
        g_cfg.posX = IniFloat(L"PosX", 20);
        g_cfg.posY = IniFloat(L"PosY", 20);
        g_cfg.fontSize = (int)IniFloat(L"FontSize", 36);
        g_cfg.moveThreshold = IniFloat(L"MoveThreshold", 0.1f);
        g_cfg.teleportDistance = IniFloat(L"TeleportDistance", 3.0f);
        g_cfg.defaultExitRadius = IniFloat(L"DefaultExitRadius", 2.5f);
        g_cfg.elevatorZone = IniString(L"ElevatorZone");
        g_cfg.truckZone = IniString(L"TruckZone");
    }

    // 初回はすべての項目を書き出して、メモ帳で編集できる状態にしておく
    void SaveConfig()
    {
        IniWrite(L"ShowTimer", g_cfg.showTimer ? L"1" : L"0");
        IniWriteFloat(L"PosX", g_cfg.posX);
        IniWriteFloat(L"PosY", g_cfg.posY);
        IniWriteFloat(L"FontSize", (float)g_cfg.fontSize);
        IniWriteFloat(L"MoveThreshold", g_cfg.moveThreshold);
        IniWriteFloat(L"TeleportDistance", g_cfg.teleportDistance);
        IniWriteFloat(L"DefaultExitRadius", g_cfg.defaultExitRadius);
        IniWrite(L"ElevatorZone", g_cfg.elevatorZone);
        IniWrite(L"TruckZone", g_cfg.truckZone);
    }

    // ---------------------------------------------------------------
    // ゲーム側のクラス・フィールド・内部関数
    // ---------------------------------------------------------------
    void* k_GameUI;
    void* k_Nolan;
    void* k_Elevator;
    void* k_Truck;
    int off_goatCountText = -1;
    int off_goatCountCanvasGroup = -1;
    int off_Text_m_Text = -1;
    const void* m_FindObjectsOfType; // Object.FindObjectsOfType(Type, bool)

    using get_transform_t = void* (*)(void*);
    using get_position_t = void (*)(void*, il2cpp::Vector3*);
    using get_alpha_t = float (*)(void*);
    using get_main_t = void* (*)();
    using get_active_scene_t = void (*)(int*);
    using scene_get_name_t = void* (*)(int);

    get_transform_t ic_get_transform;
    get_position_t ic_get_position;
    get_alpha_t ic_get_alpha;
    get_main_t ic_camera_main;
    get_active_scene_t ic_get_active_scene;
    scene_get_name_t ic_scene_get_name;

    using update_t = void (*)(void*, const void*);
    using movenext_t = bool (*)(void*, const void*);
    update_t o_GameUI_Update;
    update_t o_GameUI_OnDisable;
    movenext_t o_ShowGoatCount_MoveNext;

    // ---------------------------------------------------------------
    // 計測の状態
    // ---------------------------------------------------------------
    struct Tracked
    {
        uint32_t handle;
        il2cpp::Vector3 baseline; // 動いたかどうかの基準位置 (最初に見えた位置)
        il2cpp::Vector3 last;     // 前フレームの位置 (テレポート判定用)
    };

    std::mutex g_mutex; // 以下は描画スレッドからも読む
    Phase g_phase = Phase::Idle;
    StartMode g_mode = StartMode::Movement;
    LARGE_INTEGER g_start;
    double g_lastResult = -1;
    std::string g_sceneName;
    std::string g_lastScene;
    std::string g_startReason;
    std::vector<std::string> g_debugLines;
    std::atomic<bool> g_debug{false};

    // 描画スレッド (キー入力) → メインスレッドへの依頼
    std::atomic<bool> g_reqReset{false};
    std::atomic<bool> g_reqCenter{false};
    std::atomic<bool> g_reqRadius{false};

    // メインスレッドだけが触る
    void* g_gameUI;
    std::vector<Tracked> g_players;
    double g_nextScan;
    std::wstring g_lastGoatText;

    double Now()
    {
        static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return (double)t.QuadPart / freq.QuadPart;
    }

    double ElapsedSince(const LARGE_INTEGER& start)
    {
        static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return (double)(t.QuadPart - start.QuadPart) / freq.QuadPart;
    }

    float Horizontal(const il2cpp::Vector3& a, const il2cpp::Vector3& b)
    {
        float dx = a.x - b.x, dz = a.z - b.z;
        return std::sqrt(dx * dx + dz * dz);
    }

    float Distance(const il2cpp::Vector3& a, const il2cpp::Vector3& b)
    {
        float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    const char* ModeLabel(StartMode m)
    {
        switch (m)
        {
        case StartMode::Elevator: return "エレベーター";
        case StartMode::Truck: return "トラック";
        default: return "最初の移動";
        }
    }

    std::string FormatTime(double sec)
    {
        if (sec < 0) sec = 0;
        long long ms = (long long)(sec * 1000.0);
        int h = (int)(ms / 3600000);
        int m = (int)(ms / 60000 % 60);
        int s = (int)(ms / 1000 % 60);
        int f = (int)(ms % 1000);
        char buf[32];
        if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d.%03d", h, m, s, f);
        else snprintf(buf, sizeof(buf), "%02d:%02d.%03d", m, s, f);
        return buf;
    }

    // ---- Unity 呼び出し (メインスレッド専用) ----

    void* FindObjects(void* klass)
    {
        if (!klass || !m_FindObjectsOfType) return nullptr;
        bool includeInactive = false;
        void* args[2] = { il2cpp::TypeObject(klass), &includeInactive };
        void* exc = nullptr;
        void* arr = il2cpp::Invoke(m_FindObjectsOfType, nullptr, args, &exc);
        return exc ? nullptr : arr;
    }

    bool Position(void* component, il2cpp::Vector3& out)
    {
        if (!il2cpp::UnityAlive(component)) return false;
        void* tr = ic_get_transform(component);
        if (!il2cpp::UnityAlive(tr)) return false;
        ic_get_position(tr, &out);
        return true;
    }

    std::string ActiveSceneName()
    {
        if (!ic_get_active_scene || !ic_scene_get_name) return "unknown";
        int handle = 0;
        ic_get_active_scene(&handle);
        return il2cpp::ToUtf8(il2cpp::ReadString(ic_scene_get_name(handle)));
    }

    void ClearPlayers()
    {
        for (auto& t : g_players) il2cpp::GcHandleFree(t.handle);
        g_players.clear();
    }

    // プレイヤー (自分も他人も NolanBehaviour を持つ) を拾い直す。重いので 0.5 秒ごと。
    void ScanPlayers(bool force)
    {
        double now = Now();
        if (!force && now < g_nextScan) return;
        g_nextScan = now + 0.5;

        // 消えたプレイヤーを外す
        for (size_t i = 0; i < g_players.size();)
        {
            void* obj = il2cpp::GcHandleTarget(g_players[i].handle);
            if (!il2cpp::UnityAlive(obj))
            {
                il2cpp::GcHandleFree(g_players[i].handle);
                g_players.erase(g_players.begin() + i);
            }
            else i++;
        }

        void* arr = FindObjects(k_Nolan);
        for (size_t i = 0; i < il2cpp::ArrayLength(arr); i++)
        {
            void* obj = il2cpp::ArrayAt(arr, i);
            if (!il2cpp::UnityAlive(obj)) continue;

            bool known = false;
            for (auto& t : g_players)
            {
                if (il2cpp::GcHandleTarget(t.handle) == obj) { known = true; break; }
            }
            if (known) continue;

            il2cpp::Vector3 p;
            if (!Position(obj, p)) continue;
            g_players.push_back({ il2cpp::GcHandleNew(obj), p, p });
            Log("プレイヤー検出 (%zu人目) at (%.2f, %.2f, %.2f)", g_players.size(), p.x, p.y, p.z);
        }
    }

    // ---- エレベーター / トラックの範囲 ----

    std::wstring& ZoneValue() { return g_mode == StartMode::Truck ? g_cfg.truckZone : g_cfg.elevatorZone; }
    const wchar_t* ZoneKey() { return g_mode == StartMode::Truck ? L"TruckZone" : L"ElevatorZone"; }

    bool ParseZone(const std::wstring& raw, il2cpp::Vector3& c, float& r)
    {
        float x, z;
        if (swscanf(raw.c_str(), L"%f,%f,%f", &x, &z, &r) != 3 || r <= 0) return false;
        c = { x, 0, z };
        return true;
    }

    // 設定があればそれを、無ければスポーン位置 (最初に見えた位置) の重心を中心にする。
    // 複数人ならスポーン位置はエレベーター内に散らばるので、重心がほぼ中央になる。
    void GetZone(il2cpp::Vector3& c, float& r, bool& custom)
    {
        custom = ParseZone(ZoneValue(), c, r);
        if (custom) return;
        c = { 0, 0, 0 };
        for (auto& t : g_players) { c.x += t.baseline.x; c.z += t.baseline.z; }
        if (!g_players.empty()) { c.x /= g_players.size(); c.z /= g_players.size(); }
        r = g_cfg.defaultExitRadius;
    }

    // 自分のキャラ = カメラに一番近いプレイヤー
    bool LocalPlayerPosition(il2cpp::Vector3& out)
    {
        void* cam = ic_camera_main ? ic_camera_main() : nullptr;
        il2cpp::Vector3 cp;
        if (!Position(cam, cp)) return false;
        float best = 1e30f;
        bool found = false;
        for (auto& t : g_players)
        {
            il2cpp::Vector3 p;
            if (!Position(il2cpp::GcHandleTarget(t.handle), p)) continue;
            float d = Distance(p, cp);
            if (d < best) { best = d; out = p; found = true; }
        }
        return found;
    }

    // ---- 状態遷移 ----

    void ResetToWaiting()
    {
        ClearPlayers();
        g_lastGoatText.clear();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_phase = Phase::Waiting;
            g_startReason.clear();
        }
        ScanPlayers(true);
    }

    void EnterGame(void* gameUI)
    {
        g_gameUI = gameUI;
        StartMode mode = StartMode::Movement;
        if (il2cpp::ArrayLength(FindObjects(k_Elevator)) > 0) mode = StartMode::Elevator;
        else if (il2cpp::ArrayLength(FindObjects(k_Truck)) > 0) mode = StartMode::Truck;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_mode = mode;
            g_sceneName = ActiveSceneName();
        }
        Log("試合シーンを検出: %s / 開始条件 = %s", g_sceneName.c_str(), ModeLabel(mode));
        ResetToWaiting();
    }

    void StartTimer(const std::string& reason)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        QueryPerformanceCounter(&g_start);
        g_phase = Phase::Running;
        g_startReason = reason;
        Log("スタート: %s", reason.c_str());
    }

    void SaveResult(double sec)
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char line[512];
        snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d\t%s\t%s\t%zu人\t%s\r\n",
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
            g_sceneName.c_str(), FormatTime(sec).c_str(), g_players.size(), g_startReason.c_str());
        FILE* f = _wfopen((g_dir + L"\\DevourTimer_results.txt").c_str(), L"ab");
        if (!f) { Log("記録の保存に失敗"); return; }
        fputs(line, f);
        fclose(f);
    }

    void CheckStart()
    {
        if (g_players.empty()) return;

        // テレポート (スポーン後の配置し直し・カットシーン明け) は「動いた」に数えない
        bool rebased = false;
        for (auto& t : g_players)
        {
            il2cpp::Vector3 p;
            if (!Position(il2cpp::GcHandleTarget(t.handle), p)) continue;
            if (Distance(p, t.last) > g_cfg.teleportDistance)
            {
                Log("テレポートを検出、基準位置を取り直し: (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f)",
                    t.last.x, t.last.y, t.last.z, p.x, p.y, p.z);
                t.baseline = p;
                rebased = true;
            }
            t.last = p;
        }
        if (rebased) return;

        char reason[160];
        if (g_mode == StartMode::Movement)
        {
            for (auto& t : g_players)
            {
                float d = Horizontal(t.last, t.baseline);
                if (d > g_cfg.moveThreshold)
                {
                    snprintf(reason, sizeof(reason), "移動を検出 (%.2fm)", d);
                    StartTimer(reason);
                    return;
                }
            }
        }
        else
        {
            il2cpp::Vector3 c;
            float r;
            bool custom;
            GetZone(c, r, custom);
            for (auto& t : g_players)
            {
                float d = Horizontal(t.last, c);
                if (d > r)
                {
                    snprintf(reason, sizeof(reason), "%sから出た (中心から %.2fm > %.2fm)", ModeLabel(g_mode), d, r);
                    StartTimer(reason);
                    return;
                }
            }
        }
    }

    // 「N/M」から数字を2つ取り出す
    bool ParseCount(const std::wstring& s, int& done, int& total)
    {
        size_t slash = s.find(L'/');
        if (slash == std::wstring::npos) return false;
        size_t i = slash;
        while (i > 0 && (s[i - 1] == L' ' )) i--;
        size_t endA = i;
        while (i > 0 && iswdigit(s[i - 1])) i--;
        if (i == endA) return false;
        size_t j = slash + 1;
        while (j < s.size() && s[j] == L' ') j++;
        size_t startB = j;
        while (j < s.size() && iswdigit(s[j])) j++;
        if (j == startB) return false;
        done = _wtoi(s.substr(i, endA - i).c_str());
        total = _wtoi(s.substr(startB, j - startB).c_str());
        return true;
    }

    // 「10/10」表示 = GameUI の goatCountText と、それを包む goatCountCanvasGroup。
    // 文字が 10/10 になっていて alpha が 0 を超えた最初のフレームで止める。
    void CheckEnd()
    {
        if (!il2cpp::UnityAlive(g_gameUI)) return;
        void* text = il2cpp::Field<void*>(g_gameUI, off_goatCountText);
        void* group = il2cpp::Field<void*>(g_gameUI, off_goatCountCanvasGroup);
        if (!il2cpp::UnityAlive(text) || !il2cpp::UnityAlive(group)) return;

        std::wstring s = il2cpp::ReadString(il2cpp::Field<void*>(text, off_Text_m_Text));
        if (s != g_lastGoatText)
        {
            g_lastGoatText = s;
            Log("儀式カウント表示が変化: \"%s\"", il2cpp::ToUtf8(s).c_str());
        }

        int done, total;
        if (!ParseCount(s, done, total)) return;
        if (done < 10 || done != total) return;
        if (ic_get_alpha(group) <= 0.0f) return;

        double sec;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            sec = ElapsedSince(g_start);
            g_phase = Phase::Finished;
            g_lastResult = sec;
            g_lastScene = g_sceneName;
        }
        Log("ゴール: %s (%s)", FormatTime(sec).c_str(), g_sceneName.c_str());
        SaveResult(sec);
    }

    void HandleRequests()
    {
        if (g_reqReset.exchange(false) && g_phase != Phase::Idle)
        {
            Log("リセット: スタート待ちに戻しました");
            ResetToWaiting();
        }

        bool center = g_reqCenter.exchange(false);
        bool radius = g_reqRadius.exchange(false);
        if (!center && !radius) return;
        if (g_phase == Phase::Idle || g_mode == StartMode::Movement)
        {
            Log("F7/F8 は Asylum / Slaughterhouse の試合中だけ使えます");
            return;
        }
        il2cpp::Vector3 me;
        if (!LocalPlayerPosition(me)) return;

        il2cpp::Vector3 c;
        float r;
        bool custom;
        GetZone(c, r, custom);
        wchar_t buf[96];
        if (center)
        {
            swprintf(buf, 96, L"%.2f,%.2f,%.2f", me.x, me.z, r);
        }
        else
        {
            swprintf(buf, 96, L"%.2f,%.2f,%.2f", c.x, c.z, Horizontal(me, c));
        }
        ZoneValue() = buf;
        IniWrite(ZoneKey(), buf);
        Log("%sの%sを保存: %s", ModeLabel(g_mode), center ? "中心" : "半径", il2cpp::ToUtf8(buf).c_str());

        // 半径を測った位置はちょうど境界なので、そのままだと即スタートしてしまう。測り直し用にリセットしておく。
        if (radius) ResetToWaiting();
    }

    void BuildDebugLines()
    {
        std::vector<std::string> lines;
        char buf[256];
        const char* phases[] = { "Idle", "Waiting", "Running", "Finished" };
        snprintf(buf, sizeof(buf), "状態: %s  シーン: %s  開始条件: %s", phases[(int)g_phase], g_sceneName.c_str(), ModeLabel(g_mode));
        lines.push_back(buf);
        snprintf(buf, sizeof(buf), "プレイヤー数: %zu", g_players.size());
        lines.push_back(buf);
        if (!g_startReason.empty()) lines.push_back("開始理由: " + g_startReason);

        if (g_mode != StartMode::Movement)
        {
            il2cpp::Vector3 c;
            float r;
            bool custom;
            GetZone(c, r, custom);
            snprintf(buf, sizeof(buf), "範囲: 中心(%.2f, %.2f) 半径 %.2fm %s", c.x, c.z, r, custom ? "[設定値]" : "[自動]");
            lines.push_back(buf);
            for (auto& t : g_players)
            {
                snprintf(buf, sizeof(buf), "  中心からの距離 %.2fm", Horizontal(t.last, c));
                lines.push_back(buf);
            }
        }
        else
        {
            for (auto& t : g_players)
            {
                snprintf(buf, sizeof(buf), "  基準からの移動 %.2fm", Horizontal(t.last, t.baseline));
                lines.push_back(buf);
            }
        }

        if (il2cpp::UnityAlive(g_gameUI))
        {
            void* text = il2cpp::Field<void*>(g_gameUI, off_goatCountText);
            void* group = il2cpp::Field<void*>(g_gameUI, off_goatCountCanvasGroup);
            if (il2cpp::UnityAlive(text) && il2cpp::UnityAlive(group))
            {
                std::string s = il2cpp::ToUtf8(il2cpp::ReadString(il2cpp::Field<void*>(text, off_Text_m_Text)));
                snprintf(buf, sizeof(buf), "儀式表示: \"%s\" alpha=%.2f", s.c_str(), ic_get_alpha(group));
                lines.push_back(buf);
            }
        }

        std::lock_guard<std::mutex> lock(g_mutex);
        g_debugLines.swap(lines);
    }

    // ---------------------------------------------------------------
    // フック本体
    // ---------------------------------------------------------------

    void OnGameUIUpdate(void* self)
    {
        if (g_phase == Phase::Idle || self != g_gameUI) EnterGame(self);

        HandleRequests();

        if (g_phase == Phase::Waiting)
        {
            ScanPlayers(false);
            CheckStart();
        }
        else if (g_phase == Phase::Running)
        {
            // 本命はコルーチン側。こちらはフックが外れていた場合の保険 (1 フレーム遅れうる)
            CheckEnd();
        }

        if (g_debug) BuildDebugLines();
    }

    // ゲーム側の例外やアクセス違反で落ちないよう、判定処理は SEH で包む
    void SafeCall(void (*fn)(void*), void* arg, const char* where)
    {
        __try
        {
            fn(arg);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            static int count = 0;
            if (count++ < 5) Log("例外を握りつぶしました: %s (0x%08X)", where, GetExceptionCode());
        }
    }

    void H_GameUI_Update(void* self, const void* method)
    {
        o_GameUI_Update(self, method);
        SafeCall(OnGameUIUpdate, self, "GameUI.Update");
    }

    void LeaveGame(void* self)
    {
        if (self != g_gameUI) return;
        Log("試合シーンを抜けました");
        ClearPlayers();
        g_gameUI = nullptr;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_phase = Phase::Idle;
    }

    void H_GameUI_OnDisable(void* self, const void* method)
    {
        SafeCall(LeaveGame, self, "GameUI.OnDisable");
        o_GameUI_OnDisable(self, method);
    }

    void CheckEndIfRunning(void*)
    {
        if (g_phase == Phase::Running) CheckEnd();
    }

    // コルーチンは Update の後・LateUpdate の前に進む。
    // 進めた直後に見れば、alpha が 0 を超えた「そのフレーム」で止められる。
    bool H_ShowGoatCount_MoveNext(void* self, const void* method)
    {
        bool r = o_ShowGoatCount_MoveNext(self, method);
        SafeCall(CheckEndIfRunning, nullptr, "ShowGoatCount.MoveNext");
        return r;
    }

    bool Hook(void* target, void* detour, void** original, const char* name)
    {
        if (!target) { Log("フック先が見つからない: %s", name); return false; }
        if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK)
        {
            Log("フックに失敗: %s", name);
            return false;
        }
        Log("フック完了: %s", name);
        return true;
    }

    void* ICall(const char* name)
    {
        void* p = il2cpp::ResolveICall(name);
        if (!p) Log("icall が見つからない: %s", name);
        return p;
    }
}

bool TimerInit(const std::wstring& dir)
{
    g_dir = dir;
    g_ini = dir + L"\\DevourTimer.ini";
    LoadConfig();
    SaveConfig();

    il2cpp::AttachThread();

    k_GameUI = il2cpp::FindClass("Assembly-CSharp.dll", "", "GameUI");
    k_Nolan = il2cpp::FindClass("Assembly-CSharp.dll", "", "NolanBehaviour");
    k_Elevator = il2cpp::FindClass("Assembly-CSharp.dll", "", "ElevatorBehaviour");
    k_Truck = il2cpp::FindClass("Assembly-CSharp.dll", "", "TruckShutterBehaviour");
    void* textClass = il2cpp::FindClass("UnityEngine.UI.dll", "UnityEngine.UI", "Text");
    void* objectClass = il2cpp::FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Object");
    Log("クラス: GameUI=%p Nolan=%p Elevator=%p Truck=%p Text=%p Object=%p",
        k_GameUI, k_Nolan, k_Elevator, k_Truck, textClass, objectClass);
    if (!k_GameUI || !k_Nolan || !textClass || !objectClass) return false;

    off_goatCountText = il2cpp::FieldOffset(k_GameUI, "goatCountText");
    off_goatCountCanvasGroup = il2cpp::FieldOffset(k_GameUI, "goatCountCanvasGroup");
    off_Text_m_Text = il2cpp::FieldOffset(textClass, "m_Text");
    Log("オフセット: goatCountText=0x%X goatCountCanvasGroup=0x%X Text.m_Text=0x%X",
        off_goatCountText, off_goatCountCanvasGroup, off_Text_m_Text);
    if (off_goatCountText < 0 || off_goatCountCanvasGroup < 0 || off_Text_m_Text < 0) return false;

    m_FindObjectsOfType = il2cpp::FindMethod(objectClass, "FindObjectsOfType", 2);
    if (!m_FindObjectsOfType) { Log("Object.FindObjectsOfType(Type, bool) が見つからない"); return false; }

    ic_get_transform = (get_transform_t)ICall("UnityEngine.Component::get_transform");
    ic_get_position = (get_position_t)ICall("UnityEngine.Transform::get_position_Injected");
    ic_get_alpha = (get_alpha_t)ICall("UnityEngine.CanvasGroup::get_alpha");
    ic_camera_main = (get_main_t)ICall("UnityEngine.Camera::get_main");
    ic_get_active_scene = (get_active_scene_t)ICall("UnityEngine.SceneManagement.SceneManager::GetActiveScene_Injected");
    ic_scene_get_name = (scene_get_name_t)ICall("UnityEngine.SceneManagement.Scene::GetNameInternal");
    if (!ic_get_transform || !ic_get_position || !ic_get_alpha) return false;

    void* coroutine = il2cpp::FindNestedClass(k_GameUI, "ShowGoatCount");
    Log("ShowGoatCount コルーチン: %s", coroutine ? il2cpp::ClassName(coroutine) : "(見つからない)");

    bool ok = true;
    ok &= Hook(il2cpp::MethodPointer(il2cpp::FindMethod(k_GameUI, "Update", 0)),
        (void*)H_GameUI_Update, (void**)&o_GameUI_Update, "GameUI.Update");
    ok &= Hook(il2cpp::MethodPointer(il2cpp::FindMethod(k_GameUI, "OnDisable", 0)),
        (void*)H_GameUI_OnDisable, (void**)&o_GameUI_OnDisable, "GameUI.OnDisable");
    // これが失敗しても GameUI.Update 側の保険で止まる (1 フレーム遅れうる) ので致命的ではない
    if (coroutine)
    {
        Hook(il2cpp::MethodPointer(il2cpp::FindMethod(coroutine, "MoveNext", 0)),
            (void*)H_ShowGoatCount_MoveNext, (void**)&o_ShowGoatCount_MoveNext, "ShowGoatCount.MoveNext");
    }
    return ok;
}

void TimerSnapshotGet(TimerSnapshot& out)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    out.phase = g_phase;
    out.lastResult = g_lastResult;
    out.lastScene = g_lastScene;
    out.showTimer = g_cfg.showTimer;
    out.debug = g_debug;
    out.posX = g_cfg.posX;
    out.posY = g_cfg.posY;
    out.fontSize = g_cfg.fontSize;
    switch (g_phase)
    {
    case Phase::Running: out.elapsed = ElapsedSince(g_start); break;
    case Phase::Finished: out.elapsed = g_lastResult; break;
    case Phase::Waiting: out.elapsed = 0; break;
    default: out.elapsed = g_lastResult; break;
    }
    if (g_debug) out.debugLines = g_debugLines;
    else out.debugLines.clear();
}

int TimerFontSize() { return g_cfg.fontSize; }

// GetAsyncKeyState の下位ビット (前回以降に押されたか) はプロセス全体で共有なので使わない。
// 上位ビット (今押されているか) の立ち上がりで判定する。
void TimerHandleKeys(bool gameFocused)
{
    static bool prev[5];
    const int keys[5] = { VK_F5, VK_F6, VK_F7, VK_F8, VK_F9 };
    for (int i = 0; i < 5; i++)
    {
        bool down = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        bool pressed = down && !prev[i];
        prev[i] = down;
        if (!pressed || !gameFocused) continue;

        switch (keys[i])
        {
        case VK_F5: g_reqReset = true; break;
        case VK_F6: g_debug = !g_debug; break;
        case VK_F7: g_reqCenter = true; break;
        case VK_F8: g_reqRadius = true; break;
        case VK_F9:
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_cfg.showTimer = !g_cfg.showTimer;
            IniWrite(L"ShowTimer", g_cfg.showTimer ? L"1" : L"0");
            break;
        }
        }
    }
}

std::string TimerFormat(double sec) { return FormatTime(sec); }
