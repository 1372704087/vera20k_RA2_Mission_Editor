// VERA20K map tool -- Dear ImGui shell (Win32 + DirectX11).
// P1: open/inspect RA2 .map/.mpr/.yrm (bare INI), Chinese UI.
#include <d3d11.h>
#include <tchar.h>
#include <shellapi.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "ra2map.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "shell32.lib")

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static HRESULT g_hrHw = S_OK, g_hrWarp = S_OK;

static bool CreateDeviceD3D(HWND hwnd);
static void CleanupDeviceD3D();
static void RenderFrame();

IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static LRESULT WINAPI WndProc(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------- state

static ra2::Map g_map;
static bool g_loaded = false;
static std::string g_status = "就绪 -- 文件 -> 打开地图";

static std::string LoadFileUtf8(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::wstring ToWide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static void OpenMapDialog(HWND hwnd) {
    OPENFILENAMEW ofn = {};
    wchar_t file[MAX_PATH] = L"";
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"地图文件\0*.map;*.mpr;*.yrm;*.yrm\0所有文件\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;

    std::string data = LoadFileUtf8(ofn.lpstrFile);
    std::string err;
    if (g_map.loadFromData(data, &err)) {
        g_loaded = true;
        wchar_t name[MAX_PATH] = L"";
        wcscpy_s(name, ofn.lpstrFile);
        std::wstring fn = ofn.lpstrFile;
        std::string n8;
        int wl = lstrlenW(ofn.lpstrFile);
        int nl = WideCharToMultiByte(CP_UTF8, 0, ofn.lpstrFile, wl, nullptr, 0, nullptr, nullptr);
        n8.resize(nl);
        WideCharToMultiByte(CP_UTF8, 0, ofn.lpstrFile, wl, n8.data(), nl, nullptr, nullptr);
        g_map.path = n8;
        char buf[256];
        snprintf(buf, sizeof(buf), "已加载：%s -- %d 格，%d 个路径点",
                 g_map.name.c_str(), (int)g_map.cells.size(), (int)g_map.waypoints.size());
        g_status = buf;
    } else {
        g_loaded = false;
        g_status = "加载失败：" + err;
    }
}

// ---------------------------------------------------------------- d3d

bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };

    // hardware first; fall back to WARP software rendering (remote desktop / VM without GPU)
    g_hrHw = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &scd, &g_swap, &g_device, &fl, &g_ctx);
    if (g_hrHw != S_OK)
        g_hrWarp = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                                 levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &scd, &g_swap, &g_device, &fl, &g_ctx);
    if (g_hrHw != S_OK && g_hrWarp != S_OK) return false;

    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
    return true;
}

void CleanupDeviceD3D() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

// ---------------------------------------------------------------- ui

void RenderFrame() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("文件")) {
            if (ImGui::MenuItem("打开地图...", "Ctrl+O")) OpenMapDialog(GetActiveWindow());
            if (ImGui::MenuItem("退出")) PostQuitMessage(0);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("帮助")) {
            ImGui::Text("VERA20K 地图工具 P1");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    ImGui::TextUnformatted(g_status.c_str());
    ImGui::Separator();

    if (g_loaded) {
        if (ImGui::BeginTable("stats", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            auto row = [](const char* k, const std::string& v) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(v.c_str());
            };
            char b[128];
            snprintf(b, sizeof(b), "%s", g_map.name.c_str()); row("地图名", b);
            snprintf(b, sizeof(b), "%s", g_map.theater.c_str()); row("场景", b);
            snprintf(b, sizeof(b), "Size=%d,%d,%d,%d", g_map.size[0], g_map.size[1], g_map.size[2], g_map.size[3]);
            row("尺寸", b);
            snprintf(b, sizeof(b), "%d", (int)g_map.cells.size()); row("地形格子", b);
            snprintf(b, sizeof(b), "%d", g_map.houseCount); row("势力数", b);
            snprintf(b, sizeof(b), "%d%s", g_map.overlayBytes,
                     g_map.overlayBytes < 0 ? " (解包失败)" : ""); row("覆盖物字节", b);
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Text("路径点（%d 个，出生点为 0..N-1）", (int)g_map.waypoints.size());
        if (ImGui::BeginTable("wp", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("ID");
            ImGui::TableSetupColumn("rx");
            ImGui::TableSetupColumn("ry");
            ImGui::TableHeadersRow();
            for (auto& w : g_map.waypoints) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d", w.id);
                ImGui::TableNextColumn(); ImGui::Text("%d", w.rx);
                ImGui::TableNextColumn(); ImGui::Text("%d", w.ry);
            }
            ImGui::EndTable();
        }
    } else {
        ImGui::Dummy(ImVec2(0, 40));
        ImGui::TextWrapped("尚未加载地图。文件 -> 打开地图... 选择 .map / .mpr / .yrm（裸 INI 格式）。");
        ImGui::TextWrapped("P1 功能：地图解析与检查。批量出生点、30 势力、地形渲染在后续版本加入。");
    }

    ImGui::End();
}

// ---------------------------------------------------------------- win32

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
    case WM_SIZE:
        if (g_device && wp != SIZE_MINIMIZED) {
            if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
            g_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
            ID3D11Texture2D* back = nullptr;
            g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
            if (back) { g_device->CreateRenderTargetView(back, nullptr, &g_rtv); back->Release(); }
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // headless mode: map-tool.exe --dump <file> -> prints stats, exit 0/1
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc == 3 && wcscmp(argv[1], L"--dump") == 0) {
            std::string data = LoadFileUtf8(argv[2]);
            std::string err;
            bool ok = g_map.loadFromData(data, &err);
            fprintf(stderr, "load=%s\n", ok ? "OK" : "FAIL");
            if (!ok) fprintf(stderr, "error=%s\n", err.c_str());
            else {
                fprintf(stderr, "name=%s\ntheater=%s\n", g_map.name.c_str(), g_map.theater.c_str());
                fprintf(stderr, "size=%d,%d,%d,%d\n", g_map.size[0], g_map.size[1], g_map.size[2], g_map.size[3]);
                fprintf(stderr, "cells=%d\nhouses=%d\noverlay=%d\nwaypoints=%d\n",
                        (int)g_map.cells.size(), g_map.houseCount, g_map.overlayBytes, (int)g_map.waypoints.size());
                for (auto& w : g_map.waypoints)
                    fprintf(stderr, "wp %d %d %d\n", w.id, w.rx, w.ry);
            }
            fflush(stderr);
            LocalFree(argv);
            return ok ? 0 : 2;
        }
        LocalFree(argv);
    }

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
                       GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr,
                       nullptr, nullptr };
    wc.lpszClassName = L"Vera20kMapTool";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"VERA20K 地图工具",
                                WS_OVERLAPPEDWINDOW, 100, 100, 1280, 860,
                                nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        wchar_t msg[160];
        swprintf(msg, 160, L"D3D11 初始化失败\n硬件: 0x%08lX  WARP: 0x%08lX",
                 (unsigned long)g_hrHw, (unsigned long)g_hrWarp);
        MessageBoxW(hwnd, msg, L"错误", MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // optional: load a map from the command line (ansi path)
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc >= 2) {
            std::string data = LoadFileUtf8(argv[1]);
            std::string err;
            if (g_map.loadFromData(data, &err)) {
                g_loaded = true;
                char buf[512];
                snprintf(buf, sizeof(buf), "已加载：%s -- %d 格，%d 个路径点",
                         g_map.name.c_str(), (int)g_map.cells.size(), (int)g_map.waypoints.size());
                g_status = buf;
            } else {
                g_status = "加载失败：" + err;
            }
        }
        LocalFree(argv);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // no imgui.ini litter

    // Chinese font: Microsoft YaHei, common simplified ranges + latin
    {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 20.0f, &cfg,
                                     io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_ctx);

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderFrame();
        ImGui::Render();

        const float clear[] = {0.10f, 0.12f, 0.14f, 1.0f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
