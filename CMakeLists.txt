cmake_minimum_required(VERSION 3.20)
project(DevourTimer VERSION 1.0.1 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 配布しやすいよう VC++ ランタイムは静的リンク (相手の PC に再頒布パッケージが要らない)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/utf-8 /W3)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

# ---- MinHook ----
set(MH third_party/minhook)
add_library(minhook STATIC
    ${MH}/src/buffer.c ${MH}/src/hook.c ${MH}/src/trampoline.c ${MH}/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${MH}/include)

# ---- Dear ImGui (DX11 バックエンドだけ) ----
set(IG third_party/imgui)
add_library(imgui STATIC
    ${IG}/imgui.cpp ${IG}/imgui_draw.cpp ${IG}/imgui_tables.cpp ${IG}/imgui_widgets.cpp
    ${IG}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${IG} ${IG}/backends)
# タイマーは URL やプログラムを開かないので、ImGui 既定の ShellExecute 処理は入れない
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)

# ---- 本体 ----
set(TIMER_SOURCES src/dllmain.cpp src/il2cpp.cpp src/log.cpp src/overlay.cpp src/timer.cpp)

# DLL の正体が分かるようにバージョン情報を埋め込む (プロパティ → 詳細 で見える)
function(add_version_info target file_name)
    set(VERSION_FILE_NAME ${file_name})
    set(rc ${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc)
    configure_file(src/version.rc.in ${rc} @ONLY)
    target_sources(${target} PRIVATE ${rc})
endfunction()

# 置くだけ版: DEVOUR のフォルダに winhttp.dll として置くと、ゲーム起動時に自動で読み込まれる
add_library(winhttp_proxy SHARED ${TIMER_SOURCES} src/winhttp_proxy.cpp)
set_target_properties(winhttp_proxy PROPERTIES OUTPUT_NAME winhttp)
target_link_libraries(winhttp_proxy PRIVATE minhook imgui d3d11 dxgi)
add_version_info(winhttp_proxy winhttp.dll)

# インジェクト版: DevourTimer.exe から読み込ませる
add_library(DevourTimerNative SHARED ${TIMER_SOURCES})
target_link_libraries(DevourTimerNative PRIVATE minhook imgui d3d11 dxgi)
add_version_info(DevourTimerNative DevourTimerNative.dll)

# ---- インジェクター ----
add_executable(DevourTimer injector/injector.cpp)

# ビルドしたら dist/ に 2 つ並べる (配布はこのフォルダを zip にするだけ)
set(DIST ${CMAKE_SOURCE_DIR}/dist)
add_custom_command(TARGET DevourTimer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:DevourTimer> ${DIST})
add_custom_command(TARGET DevourTimerNative POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:DevourTimerNative> ${DIST})
# 置くだけ版は別フォルダに (exe と並べると紛らわしいので)
add_custom_command(TARGET winhttp_proxy POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST}/drop-in
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:winhttp_proxy> ${DIST}/drop-in)
