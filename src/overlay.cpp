// D3D11 の Present をフックして、画面にタイマーを重ねて描く。
// Present の場所は「捨てのデバイスとスワップチェーンを作って vtable を覗く」いつもの方法で取る。
// 描画だけで入力は奪わないので、ImGui の Win32 バックエンドは使わない (カーソルにも触らない)。
#include "overlay.h"
#include "timer.h"
#include "log.h"

#include <windows.h>
#include <d3d11.h>
#include <MinHook.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"

namespace
{
    using present_t = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
    using resize_t = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

    present_t o_Present;
    resize_t o_ResizeBuffers;

    bool g_ready;
    HWND g_hwnd;
    ID3D11Device* g_device;
    ID3D11DeviceContext* g_context;
    ID3D11RenderTargetView* g_rtv;
    ImFont* g_timerFont;
    ImFont* g_smallFont;
    LARGE_INTEGER g_freq, g_lastFrame;

    void CreateRtv(IDXGISwapChain* sc)
    {
        ID3D11Texture2D* back = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back)
        {
            g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
            back->Release();
        }
    }

    bool InitImGui(IDXGISwapChain* sc)
    {
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_device))) return false;
        g_device->GetImmediateContext(&g_context);
        DXGI_SWAP_CHAIN_DESC desc;
        sc->GetDesc(&desc);
        g_hwnd = desc.OutputWindow;

        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;

        // タイマー用の等幅太字と、デバッグ表示用の日本語フォント。無ければ ImGui 内蔵フォント
        float size = (float)TimerFontSize();
        g_timerFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consolab.ttf", size);
        if (!g_timerFont)
        {
            ImFontConfig cfg;
            cfg.SizePixels = size;
            g_timerFont = io.Fonts->AddFontDefault(&cfg);
        }
        g_smallFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\meiryo.ttc", 16.0f, nullptr,
            io.Fonts->GetGlyphRangesJapanese());
        if (!g_smallFont) g_smallFont = io.Fonts->AddFontDefault();

        ImGui::StyleColorsDark();
        ImGui::GetStyle().WindowRounding = 4.0f;
        ImGui::GetStyle().WindowBorderSize = 0.0f;

        ImGui_ImplDX11_Init(g_device, g_context);
        CreateRtv(sc);

        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_lastFrame);
        Log("オーバーレイ初期化完了 (hwnd=%p)", g_hwnd);
        return true;
    }

    void Draw()
    {
        TimerSnapshot s;
        TimerSnapshotGet(s);

        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

        float y = s.posY;
        if (s.showTimer && !(s.phase == Phase::Idle && s.lastResult < 0))
        {
            ImVec4 color = s.phase == Phase::Running ? ImVec4(1, 1, 1, 1)
                : s.phase == Phase::Finished ? ImVec4(0.4f, 1, 0.4f, 1)
                : ImVec4(0.7f, 0.7f, 0.7f, 1);

            ImGui::SetNextWindowPos(ImVec2(s.posX, s.posY));
            ImGui::SetNextWindowBgAlpha(0.55f);
            ImGui::Begin("##timer", nullptr, flags);
            ImGui::PushFont(g_timerFont);
            ImGui::TextColored(color, "%s", TimerFormat(s.elapsed).c_str());
            ImGui::PopFont();
            if (s.phase == Phase::Idle)
            {
                ImGui::PushFont(g_smallFont);
                ImGui::Text("前回: %s", s.lastScene.c_str());
                ImGui::PopFont();
            }
            y += ImGui::GetWindowHeight() + 8;
            ImGui::End();
        }

        if (s.debug)
        {
            ImGui::SetNextWindowPos(ImVec2(s.posX, y));
            ImGui::SetNextWindowBgAlpha(0.55f);
            ImGui::Begin("##debug", nullptr, flags);
            ImGui::PushFont(g_smallFont);
            if (s.debugLines.empty()) ImGui::Text("試合シーン待ち (GameUI がまだ動いていない)");
            for (auto& line : s.debugLines) ImGui::TextUnformatted(line.c_str());
            ImGui::PopFont();
            ImGui::End();
        }
    }

    HRESULT __stdcall H_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
    {
        if (!g_ready) g_ready = InitImGui(sc);

        if (g_ready && g_rtv)
        {
            TimerHandleKeys(GetForegroundWindow() == g_hwnd);

            ImGuiIO& io = ImGui::GetIO();
            RECT rc;
            GetClientRect(g_hwnd, &rc);
            io.DisplaySize = ImVec2((float)(rc.right - rc.left), (float)(rc.bottom - rc.top));
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            float dt = (float)(now.QuadPart - g_lastFrame.QuadPart) / g_freq.QuadPart;
            io.DeltaTime = dt > 0 ? dt : 1.0f / 60.0f;
            g_lastFrame = now;

            ImGui_ImplDX11_NewFrame();
            ImGui::NewFrame();
            Draw();
            ImGui::Render();
            g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
        return o_Present(sc, sync, flags);
    }

    // 解像度変更のときはバックバッファを握ったままだと失敗するので、いったん手放す
    HRESULT __stdcall H_ResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
    {
        if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
        HRESULT hr = o_ResizeBuffers(sc, count, w, h, fmt, flags);
        if (g_ready) CreateRtv(sc);
        return hr;
    }

    bool GetSwapChainVTable(void**& vtable)
    {
        WNDCLASSEXW wc = { sizeof(wc), CS_HREDRAW | CS_VREDRAW, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr),
            nullptr, nullptr, nullptr, nullptr, L"DevourTimerDummy", nullptr };
        RegisterClassExW(&wc);
        HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 1;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        IDXGISwapChain* sc = nullptr;
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        D3D_FEATURE_LEVEL level;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &sd, &sc, &dev, &level, &ctx);

        static void* copy[18];
        bool ok = SUCCEEDED(hr) && sc;
        if (ok)
        {
            memcpy(copy, *reinterpret_cast<void***>(sc), sizeof(copy));
            vtable = copy;
        }
        else Log("ダミーのスワップチェーン作成に失敗 (0x%08X)", (unsigned)hr);

        if (sc) sc->Release();
        if (ctx) ctx->Release();
        if (dev) dev->Release();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return ok;
    }
}

bool OverlayInit()
{
    void** vt = nullptr;
    if (!GetSwapChainVTable(vt)) return false;

    // IDXGISwapChain の vtable: 8 = Present, 13 = ResizeBuffers
    if (MH_CreateHook(vt[8], (void*)H_Present, (void**)&o_Present) != MH_OK ||
        MH_CreateHook(vt[13], (void*)H_ResizeBuffers, (void**)&o_ResizeBuffers) != MH_OK)
    {
        Log("Present / ResizeBuffers のフック作成に失敗");
        return false;
    }
    MH_EnableHook(vt[8]);
    MH_EnableHook(vt[13]);
    Log("フック完了: IDXGISwapChain::Present / ResizeBuffers");
    return true;
}
