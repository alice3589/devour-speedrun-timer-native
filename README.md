# DEVOUR Speedrun Timer（DLLインジェクト版）

DEVOUR の speedrun.com ルールどおりに計測し、ゲーム画面にタイマーを表示します。
**MelonLoader 不要**。ゲームのフォルダには何も入れません。

## 使い方

1. `DevourTimer.exe` と `DevourTimerNative.dll` を同じフォルダに置く（どこでもよい）
2. `DevourTimer.exe` をダブルクリック
   - DEVOUR が起動していなければ Steam で起動します
   - ゲームの準備ができたら自動でタイマーを読み込みます
3. 画面左上にタイマーが出ます

ゲームを起動するたびに 2 をやってください（ゲームを閉じるとタイマーも外れます）。

| キー | 動作 |
|---|---|
| `F5` | リセット（スタート待ちに戻す） |
| `F6` | デバッグ表示 |
| `F7` | 今いる位置をエレベーター/トラックの中心として保存（Asylum / Slaughterhouse のみ） |
| `F8` | 中心から今いる位置までの距離を半径として保存（出口の線の上で押す） |
| `F9` | タイマー表示 ON/OFF |

## 計測ルール

| ルール | 判定方法 |
|---|---|
| 最初の移動で開始（通常マップ） | 誰か1人が水平に `MoveThreshold`(0.1m) 以上動いた瞬間。視点移動・ライト・カットシーンスキップでは始まらない |
| Asylum：最初の人がエレベーターを出たら開始 | エレベーターの範囲の外に出た瞬間。中で動くのは OK |
| Slaughterhouse：最初の人がトラックを出たら開始 | 同上 |
| 10個目の儀式後、「10/10」がフェードインし始めた最初のフレームで終了 | 「N/10」をフェードさせるコルーチンを進めた直後に、文字が `10/10` かつ透明度 > 0 になったフレームで止める |

実時間（QueryPerformanceCounter）で計測。ポーズしても止まりません。

## 仕組み

`GameAssembly.dll` が公開している il2cpp の C API でゲームのクラスを引き、関数をフックしています。

| フック先 | 用途 |
|---|---|
| `GameUI.Update` | 毎フレームのスタート判定（Unity の API はメインスレッドでしか呼べないため、ここで行う） |
| `GameUI.<ShowGoatCount>d__N.MoveNext` | ゴール判定。コルーチンは Update の後に進むので、フェードしたそのフレームで止められる |
| `GameUI.OnDisable` | 試合シーンを抜けた（待機に戻る） |
| `IDXGISwapChain::Present` | Dear ImGui でタイマーを描画 |

クラスやフィールドは名前で引いているので、ゲームのアップデートでコンパイラが付ける番号（`d__404` → `d__406` など）が変わっても動きます。

## ファイル（exe と同じフォルダに作られる）

| ファイル | 内容 |
|---|---|
| `DevourTimer.ini` | 設定。初回起動時に全項目が書き出されます |
| `DevourTimer_results.txt` | ゴールするたびに 1 行追記（日時・シーン・タイム・人数・開始理由） |
| `DevourTimer.log` | 読み込み・フック・スタート/ゴールの記録。表示されないときはここを見る |

### 設定項目（`DevourTimer.ini` の `[Timer]`）

| キー | 既定 | 説明 |
|---|---|---|
| `ShowTimer` | 1 | タイマーを表示する |
| `PosX` / `PosY` | 20 / 20 | 表示位置 |
| `FontSize` | 36 | 文字サイズ（ゲーム再起動で反映） |
| `MoveThreshold` | 0.1 | 「動いた」とみなす水平移動量(m) |
| `TeleportDistance` | 3 | 1フレームでこれ以上(m)位置が飛んだら、移動ではなくゲーム側の配置し直しとみなす |
| `ElevatorZone` | (空) | Asylum の範囲 `中心X,中心Z,半径` |
| `TruckZone` | (空) | Slaughterhouse の範囲 |
| `DefaultExitRadius` | 2.5 | 範囲が未設定のときの半径 |

## Asylum / Slaughterhouse の範囲合わせ（最初に1回）

範囲が未設定だと「スポーン位置の重心から半径 2.5m」を使います。これは仮の値なので、一度合わせてください。

1. ソロで Asylum を始め、`F6` でデバッグ表示を出す
2. エレベーターの中央に立って `F7`
3. 「エレベーターを出た」とみなす境目に立って `F8`（タイマーはスタート待ちに戻ります）
4. Slaughterhouse も同じ手順で

## ビルド

Visual Studio 2022（C++ デスクトップ開発）と CMake が必要です。

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

`dist\` に `DevourTimer.exe` と `DevourTimerNative.dll` が並びます。配布はこのフォルダを zip にするだけです。
VC++ ランタイムは静的リンクしているので、相手の PC に再頒布パッケージは要りません。

同梱ライブラリ: [MinHook](https://github.com/TsudaKageyu/minhook) v1.3.4（BSD-2）、
[Dear ImGui](https://github.com/ocornut/imgui) v1.91.9（MIT）。`third_party/` にライセンスごと入っています。

## 注意

- **走るときは MelonLoader 版のタイマーと ESP（`Mods\` の DLL）を外してください。** 両方入っていると F キーに両方が反応します
- マルチプレイでは他人の位置は通信で届くので、他人が先に動いた場合は自分の画面で数十〜100ms程度開始が遅れることがあります。ホストのタイマーが最も正確です
- ウイルス対策ソフトがインジェクターを誤検知することがあります（他プロセスに DLL を読み込ませる動作のため）
- 確認済み: 読み込み・全フックの成功・メニュー画面での描画。**実際の試合での開始・終了判定はまだ未確認**
