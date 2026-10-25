#pragma once
#include <string>
#include <vector>

enum class Phase { Idle, Waiting, Running, Finished };
enum class StartMode { Movement, Elevator, Truck };

// 描画スレッドへ渡す、ある瞬間の状態のコピー
struct TimerSnapshot
{
    Phase phase = Phase::Idle;
    double elapsed = 0;       // 表示する秒数
    double lastResult = -1;   // 直前の記録 (無ければ -1)
    std::string lastScene;
    bool showTimer = true;
    bool debug = false;
    float posX = 20, posY = 20;
    int fontSize = 36;
    std::vector<std::string> debugLines;
};

// ゲーム内のクラスを解決してフックを入れる。dir は DLL のあるフォルダ (設定・記録の置き場)
bool TimerInit(const std::wstring& dir);

// 描画スレッドから毎フレーム呼ぶ
void TimerSnapshotGet(TimerSnapshot& out);
void TimerHandleKeys(bool gameFocused);
int TimerFontSize();
std::string TimerFormat(double sec);
