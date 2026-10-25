// VERA20K map editor -- Dear ImGui (Win32 + DirectX11).
// P1: open/inspect RA2 .map/.mpr/.yrm (bare INI), Chinese UI.
// P3a: terrain viewport (theater tiles from game MIX archives), pan/zoom,
//      waypoint pick & drag.
#include <d3d11.h>
#include <tchar.h>
#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "ra2map.h"
#include "gamefiles.h"
#include "theater.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

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

static ra2::GameAssets* g_assets = nullptr;
static ra2::TheaterData* g_theater = nullptr;
static std::string g_theaterError;

// atlas: 64x64 cells on a 2048x2048 RGBA texture (32x32 grid = 1024 slots)
static constexpr int ATLAS_TEX = 2048;
static constexpr int ATLAS_CELL = 64;
static constexpr int ATLAS_GRID = ATLAS_TEX / ATLAS_CELL;
static ID3D11ShaderResourceView* g_atlasSrv = nullptr;
static std::map<uint64_t, int> g_atlasSlot;   // (tileId<<8|subtile) -> slot
static std::vector<uint8_t> g_atlasRgba;      // CPU staging copy (ATLAS_TEX^2 * 4)
static bool g_atlasDirty = false;

static std::vector<int> g_drawOrder;          // cell indices sorted by (x+y, x)
static int g_minX = 0, g_maxX = 0, g_minY = 0, g_maxY = 0;

struct View {
    float zoom = 1.0f;
    ImVec2 pan = ImVec2(0, 0);
    bool needsCenter = true;
    int selectedWp = -1;
    bool draggingWp = false;
    int hoverCellX = -1, hoverCellY = -1;
    int hoverTile = -1, hoverHeight = -1;
};
static View g_view;

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

// ---------------------------------------------------------------- game dir config

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring d = buf;
    size_t p = d.find_last_of(L"/\\");
    return p == std::wstring::npos ? L"." : d.substr(0, p);
}

static std::wstring g_gameDir;

static void LoadConfig() {
    std::string t = LoadFileUtf8(ExeDir() + L"\\map-tool.cfg");
    if (t.rfind("gameDir=", 0) == 0) {
        std::wstring d = ToWide(t.substr(8));
        while (!d.empty() && (d.back() == L'\r' || d.back() == L'\n')) d.pop_back();
        g_gameDir = d;
    }
}

// (simple writer to avoid encoding pitfalls)
static void WriteConfig() {
    FILE* f = _wfopen((ExeDir() + L"\\map-tool.cfg").c_str(), L"wb");
    if (!f) return;
    std::string line = "gameDir=";
    int n = WideCharToMultiByte(CP_UTF8, 0, g_gameDir.c_str(), (int)g_gameDir.size(), nullptr, 0, nullptr, nullptr);
    line.resize(line.size() + n);
    WideCharToMultiByte(CP_UTF8, 0, g_gameDir.c_str(), (int)g_gameDir.size(), line.data() + 8, n, nullptr, nullptr);
    fwrite(line.data(), 1, line.size(), f);
    fputc('\n', f);
    fclose(f);
}

static bool PickFolder(HWND owner, std::wstring& out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return false;
    dlg->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                out = p;
                CoTaskMemFree(p);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

static bool EnsureAssets(std::string* err) {
    if (g_assets) return true;
    // candidates: configured dir, common install paths
    std::vector<std::wstring> candidates;
    if (!g_gameDir.empty()) candidates.push_back(g_gameDir);
    candidates.push_back(L"H:\\Red-Alert-2-Yuris-Revenge");
    candidates.push_back(L"H:\\测试\\Red-Alert-2-Yuris-Revenge");
    candidates.push_back(L"C:\\Westwood\\RA2");
    for (auto& dir : candidates) {
        DWORD attr = GetFileAttributesW(dir.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
        ra2::GameAssets* a = new ra2::GameAssets();
        std::string e;
        if (a->open(dir, &e)) {
            g_assets = a;
            g_gameDir = dir;
            return true;
        }
        delete a;
        if (err) *err = e;
    }
    if (err) *err = "找不到游戏目录（需要包含 .mix 文件的红警2安装）";
    return false;
}

static void ReleaseTheater() {
    delete g_theater;
    g_theater = nullptr;
    g_theaterError.clear();
    if (g_atlasSrv) { g_atlasSrv->Release(); g_atlasSrv = nullptr; }
    g_atlasSlot.clear();
    g_atlasRgba.clear();
}

// ---------------------------------------------------------------- map loading helpers

static void PrepareMapView() {
    g_drawOrder.clear();
    if (g_map.cells.empty()) return;
    g_drawOrder.resize(g_map.cells.size());
    for (int i = 0; i < (int)g_map.cells.size(); i++) g_drawOrder[i] = i;
    const auto& cs = g_map.cells;
    g_minX = cs[0].x; g_maxX = cs[0].x;
    g_minY = cs[0].y; g_maxY = cs[0].y;
    for (auto& c : cs) {
        if (c.x < g_minX) g_minX = c.x;
        if (c.x > g_maxX) g_maxX = c.x;
        if (c.y < g_minY) g_minY = c.y;
        if (c.y > g_maxY) g_maxY = c.y;
    }
    std::sort(g_drawOrder.begin(), g_drawOrder.end(), [&cs](int a, int b) {
        int sa = cs[a].x + cs[a].y, sb = cs[b].x + cs[b].y;
        if (sa != sb) return sa < sb;
        return cs[a].x < cs[b].x;
    });
    g_view.needsCenter = true;
}

static bool EnsureTheater() {
    if (g_theater) return true;
    if (!g_loaded) return false;
    std::string err;
    if (!EnsureAssets(&err)) {
        g_theaterError = err;
        return false;
    }
    bool yuri = g_map.path.size() > 4 &&
                _stricmp(g_map.path.c_str() + g_map.path.size() - 4, ".yrm") == 0;
    g_theater = ra2::TheaterData::load(g_assets, g_map.theater, yuri, &g_theaterError);
    if (g_theater) {
        g_atlasDirty = true;
        return true;
    }
    return false;
}

static void BlitTileToAtlas(const ra2::TmpFile& tmp, int subtile, int slot) {
    if (subtile < 0 || subtile >= (int)tmp.tiles.size()) return;
    const ra2::TmpTile& t = tmp.tiles[subtile];
    if (!t.present) return;
    int cellX = (slot % ATLAS_GRID) * ATLAS_CELL;
    int cellY = (slot / ATLAS_GRID) * ATLAS_CELL;
    const uint8_t* pal = g_theater->isoPalette();
    int ox = 2 + t.x;               // diamond left edge inside the 64px cell
    int oy = ATLAS_CELL - tmp.cy + t.y;
    for (int y = 0; y < tmp.cy; y++) {
        for (int x = 0; x < tmp.cx; x++) {
            uint8_t idx = t.pixels[(size_t)y * tmp.cx + x];
            if (!idx) continue;
            int px = cellX + ox + x, py = cellY + oy + y;
            if (px < cellX || px >= cellX + ATLAS_CELL || py < cellY || py >= cellY + ATLAS_CELL) continue;
            uint8_t* d = &g_atlasRgba[((size_t)py * ATLAS_TEX + px) * 4];
            d[0] = pal[idx * 3]; d[1] = pal[idx * 3 + 1]; d[2] = pal[idx * 3 + 2]; d[3] = 255;
        }
    }
    if (t.hasExtra && !t.extra.empty() && t.cxExtra > 0 && t.cyExtra > 0) {
        for (int y = 0; y < t.cyExtra; y++) {
            for (int x = 0; x < t.cxExtra; x++) {
                uint8_t idx = t.extra[(size_t)y * t.cxExtra + x];
                if (!idx) continue;
                int px = cellX + ox + t.xExtra + x;
                int py = cellY + oy + t.yExtra + y;
                if (px < cellX || px >= cellX + ATLAS_CELL || py < cellY || py >= cellY + ATLAS_CELL) continue;
                uint8_t* d = &g_atlasRgba[((size_t)py * ATLAS_TEX + px) * 4];
                d[0] = pal[idx * 3]; d[1] = pal[idx * 3 + 1]; d[2] = pal[idx * 3 + 2]; d[3] = 255;
            }
        }
    }
}

static void BuildAtlasIfNeeded() {
    if (!g_atlasDirty || !g_theater || !g_device) return;
    g_atlasDirty = false;

    if (g_atlasSrv) { g_atlasSrv->Release(); g_atlasSrv = nullptr; }
    g_atlasSlot.clear();
    g_atlasRgba.assign((size_t)ATLAS_TEX * ATLAS_TEX * 4, 0);

    int nextSlot = 0;
    for (int ci : g_drawOrder) {
        const auto& c = g_map.cells[ci];
        uint64_t key = ((uint64_t)(uint32_t)c.tile << 8) | c.subtile;
        if (g_atlasSlot.count(key)) continue;
        if (nextSlot >= ATLAS_GRID * ATLAS_GRID) break;
        ra2::TmpFile* tmp = g_theater->loadTmp(c.tile);
        if (!tmp) continue;
        g_atlasSlot[key] = nextSlot;
        BlitTileToAtlas(*tmp, c.subtile, nextSlot);
        nextSlot++;
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = ATLAS_TEX; td.Height = ATLAS_TEX; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = g_atlasRgba.data();
    sd.SysMemPitch = ATLAS_TEX * 4;
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&td, &sd, &tex)) && tex) {
        D3D11_SHADER_RESOURCE_VIEW_DESC rv = {};
        rv.Format = td.Format;
        rv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        rv.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(tex, &rv, &g_atlasSrv);
        tex->Release();
    }
    g_atlasRgba.clear(); // uploaded
    g_atlasRgba.shrink_to_fit();
}

// ---------------------------------------------------------------- open dialog

static void OpenMapDialog(HWND hwnd) {
    OPENFILENAMEW ofn = {};
    wchar_t file[MAX_PATH] = L"";
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"地图文件\0*.map;*.mpr;*.yrm\0所有文件\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;

    std::string data = LoadFileUtf8(ofn.lpstrFile);
    std::string err;
    if (g_map.loadFromData(data, &err)) {
        g_loaded = true;
        int wl = lstrlenW(ofn.lpstrFile);
        int nl = WideCharToMultiByte(CP_UTF8, 0, ofn.lpstrFile, wl, nullptr, 0, nullptr, nullptr);
        g_map.path.resize(nl);
        WideCharToMultiByte(CP_UTF8, 0, ofn.lpstrFile, wl, g_map.path.data(), nl, nullptr, nullptr);
        ReleaseTheater();
        EnsureTheater();
        PrepareMapView();
        char buf[256];
        snprintf(buf, sizeof(buf), "已加载：%s -- %d 格，%d 个路径点%s%s",
                 g_map.name.c_str(), (int)g_map.cells.size(), (int)g_map.waypoints.size(),
                 g_theater ? " -- " : "",
                 g_theater ? g_theater->theater().c_str() : (g_theaterError.empty() ? "" : g_theaterError.c_str()));
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

// ---------------------------------------------------------------- viewport

static void RenderViewport(float availW, float availH) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.07f, 0.08f, 1));
    ImGui::BeginChild("mapview", ImVec2(availW, availH), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoMove);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    bool hovered = ImGui::IsWindowHovered();

    float z = g_view.zoom;
    float hw = 30.0f * z;  // half tile width
    float hh = 15.0f * z;  // half tile height

    // center on first frame
    if (g_view.needsCenter) {
        g_view.needsCenter = false;
        float cx = (g_minX - g_minY + (g_maxX - g_maxY)) / 2.0f * hw;
        float cy = (g_minX + g_minY + g_maxX + g_maxY) / 4.0f * hh;
        g_view.pan = ImVec2(size.x / 2 - cx, size.y / 2 - cy);
    }

    auto cellToScreen = [&](int x, int y, int h, ImVec2& out) {
        out.x = p0.x + g_view.pan.x + (x - y) * hw;
        out.y = p0.y + g_view.pan.y + (x + y) * hh - h * hh;
    };
    auto screenToCellApprox = [&](const ImVec2& m, int& x, int& y) {
        float mx = (m.x - p0.x - g_view.pan.x) / hw;
        float my = (m.y - p0.y - g_view.pan.y) / hh;
        x = (int)floor((my + mx) / 2);
        y = (int)floor((my - mx) / 2);
    };

    ImVec2 mouse = io.MousePos;
    if (hovered && !io.WantTextInput) {
        // zoom to cursor
        float wheel = io.MouseWheel;
        if (wheel != 0) {
            int hx, hy;
            screenToCellApprox(mouse, hx, hy);
            float nz = g_view.zoom * (wheel > 0 ? 1.15f : 1 / 1.15f);
            if (nz < 0.2f) nz = 0.2f;
            if (nz > 6.0f) nz = 6.0f;
            g_view.zoom = nz;
            z = nz; hw = 30 * z; hh = 15 * z;
            ImVec2 after;
            cellToScreen(hx, hy, 0, after);
            g_view.pan.x += (mouse.x - after.x);
            g_view.pan.y += (mouse.y - after.y);
        }
        // pan with right/middle drag
        if (io.MouseDown[1] || io.MouseDown[2]) {
            g_view.pan.x += io.MouseDelta.x;
            g_view.pan.y += io.MouseDelta.y;
        }
        // hover cell (approximate: height ignored for the inverse transform)
        screenToCellApprox(mouse, g_view.hoverCellX, g_view.hoverCellY);
        g_view.hoverTile = -1;
        g_view.hoverHeight = -1;
        static int frame = 0;
        if (++frame % 5 == 0 && g_loaded) {
            int wantX = g_view.hoverCellX, wantY = g_view.hoverCellY;
            for (size_t i = 0; i < g_map.cells.size(); i++) {
                const auto& c = g_map.cells[i];
                if (c.x == wantX && c.y == wantY) {
                    g_view.hoverTile = c.tile;
                    g_view.hoverHeight = c.height;
                    break;
                }
            }
        }

        // waypoint interaction: pick on click, drag to move
        float bestDist = 20 * 20;
        int hitWp = -1;
        ImVec2 hitPos;
        for (const auto& w : g_map.waypoints) {
            // find cell height at waypoint (approx via cached cells; 0 if unknown)
            ImVec2 pos;
            cellToScreen(w.rx, w.ry, 0, pos);
            pos.x += hw; pos.y += hh; // diamond center
            float dx = mouse.x - pos.x, dy = mouse.y - pos.y;
            float d2 = dx * dx + dy * dy;
            if (d2 < bestDist) {
                bestDist = d2;
                hitWp = w.id;
                hitPos = pos;
            }
        }
        if (ImGui::IsMouseClicked(0) && hitWp >= 0) {
            g_view.selectedWp = hitWp;
            g_view.draggingWp = true;
        }
        if (!io.MouseDown[0]) g_view.draggingWp = false;
        if (g_view.draggingWp && g_view.selectedWp >= 0) {
            int nx, ny;
            screenToCellApprox(mouse, nx, ny);
            for (auto& w : g_map.waypoints) {
                if (w.id == g_view.selectedWp) {
                    w.rx = nx;
                    w.ry = ny;
                    break;
                }
            }
        }
    }

    // ---- terrain
    if (g_loaded && !g_drawOrder.empty()) {
        BuildAtlasIfNeeded();
        float pad = ATLAS_CELL * z;
        ImTextureID tex = (ImTextureID)g_atlasSrv;
        for (int ci : g_drawOrder) {
            const auto& c = g_map.cells[ci];
            ImVec2 sp;
            cellToScreen(c.x, c.y, c.height, sp);
            if (sp.x < p0.x - pad || sp.x > p0.x + size.x + pad ||
                sp.y < p0.y - pad || sp.y > p0.y + size.y + pad)
                continue;
            if (g_atlasSrv && c.tile >= 0) {
                uint64_t key = ((uint64_t)(uint32_t)c.tile << 8) | c.subtile;
                auto it = g_atlasSlot.find(key);
                if (it != g_atlasSlot.end()) {
                    int slot = it->second;
                    float u0 = (float)((slot % ATLAS_GRID) * ATLAS_CELL) / ATLAS_TEX;
                    float v0 = (float)((slot / ATLAS_GRID) * ATLAS_CELL) / ATLAS_TEX;
                    float u1 = u0 + (float)ATLAS_CELL / ATLAS_TEX;
                    float v1 = v0 + (float)ATLAS_CELL / ATLAS_TEX;
                    // atlas cell 64x64: diamond sits at bottom-left with offsets (2, 34)
                    ImVec2 pmin(sp.x - 2 * z, sp.y - (ATLAS_CELL - 30) * z);
                    dl->AddImage(tex, pmin, ImVec2(pmin.x + ATLAS_CELL * z, pmin.y + ATLAS_CELL * z),
                                 ImVec2(u0, v0), ImVec2(u1, v1));
                    continue;
                }
            }
            // fallback: flat shaded diamond
            ImVec2 a(sp.x, sp.y), b(sp.x + hw, sp.y + hh), c2(sp.x + 2 * hw, sp.y), d(sp.x + hw, sp.y - hh);
            float g = 0.25f + 0.05f * (c.height % 8);
            dl->AddQuadFilled(a, b, c2, d, ImColor(g, g + 0.02f, g, 1.0f));
        }
    }

    // ---- waypoints
    if (g_loaded) {
        char label[16];
        for (const auto& w : g_map.waypoints) {
            ImVec2 pos;
            cellToScreen(w.rx, w.ry, 0, pos);
            pos.x += hw; pos.y += hh;
            if (pos.x < p0.x - 64 || pos.x > p0.x + size.x + 64 ||
                pos.y < p0.y - 64 || pos.y > p0.y + size.y + 64)
                continue;
            bool player = w.id < 30;
            bool sel = w.id == g_view.selectedWp;
            ImU32 col = sel ? IM_COL32(255, 200, 0, 255)
                        : player ? IM_COL32(80, 220, 120, 255) : IM_COL32(160, 160, 200, 255);
            ImU32 fill = sel ? IM_COL32(255, 200, 0, 70)
                          : player ? IM_COL32(80, 220, 120, 70) : IM_COL32(160, 160, 200, 70);
            float r = ImGui::GetTextLineHeight() * 0.5f;
            dl->AddCircleFilled(pos, r, fill);
            dl->AddCircle(pos, r, col, 12, 2.0f);
            snprintf(label, sizeof(label), "%d", w.id);
            ImVec2 tsz = ImGui::CalcTextSize(label);
            dl->AddText(ImVec2(pos.x - tsz.x / 2, pos.y - tsz.y / 2),
                        sel ? IM_COL32(255, 240, 160, 255) : IM_COL32(240, 240, 240, 255), label);
        }
    }

    // ---- hover marker + status
    if (g_loaded && hovered && g_view.hoverCellX >= 0) {
        ImVec2 a, b, c2, d;
        cellToScreen(g_view.hoverCellX, g_view.hoverCellY, g_view.hoverHeight >= 0 ? g_view.hoverHeight : 0, a);
        b = ImVec2(a.x + hw, a.y + hh);
        c2 = ImVec2(a.x + 2 * hw, a.y);
        d = ImVec2(a.x + hw, a.y - hh);
        dl->AddQuad(a, b, c2, d, ImColor(255, 255, 255, 140), 2.0f);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    // status under the viewport
    char st[256];
    if (g_loaded) {
        snprintf(st, sizeof(st), "格 (%d,%d)  %s%s  缩放 %.2f  %s",
                 g_view.hoverCellX, g_view.hoverCellY,
                 g_view.hoverTile >= 0 ? "瓦片=" : "",
                 g_view.hoverTile >= 0 ? std::to_string(g_view.hoverTile).c_str() : "",
                 g_view.zoom,
                 g_theater ? "" : (g_theaterError.empty() ? "" : g_theaterError.c_str()));
    } else {
        snprintf(st, sizeof(st), "未加载地图");
    }
    ImGui::TextUnformatted(st);
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
        if (ImGui::BeginMenu("资源")) {
            if (ImGui::MenuItem("选择游戏目录...")) {
                std::wstring dir;
                if (PickFolder(GetActiveWindow(), dir)) {
                    g_gameDir = dir;
                    WriteConfig();
                    delete g_assets;
                    g_assets = nullptr;
                    ReleaseTheater();
                    EnsureTheater();
                }
            }
            ImGui::TextUnformatted(g_gameDir.empty() ? "(未设置)" : (std::string(g_gameDir.begin(), g_gameDir.end())).c_str());
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("帮助")) {
            ImGui::Text("VERA20K 地图编辑器");
            ImGui::Text("滚轮缩放 / 右键拖动平移 / 拖动路径点");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    if (!g_loaded) {
        ImGui::Dummy(ImVec2(0, 40));
        ImGui::TextWrapped("尚未加载地图。文件 -> 打开地图... 选择 .map / .mpr / .yrm（裸 INI 格式）。");
        ImGui::TextWrapped("地形渲染需要游戏目录（含 .mix 文件）：资源 -> 选择游戏目录... ");
        ImGui::End();
        return;
    }

    // left panel
    float panelW = 300.0f;
    ImGui::BeginChild("side", ImVec2(panelW, 0), true);
    ImGui::TextUnformatted(g_status.c_str());
    ImGui::Separator();
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
        if (g_theater) {
            snprintf(b, sizeof(b), "%d", g_theater->tileCount()); row("瓦片集数", b);
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Text("路径点（%d 个）", (int)g_map.waypoints.size());
    if (ImGui::BeginTable("wp", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                         ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("ID");
        ImGui::TableSetupColumn("rx");
        ImGui::TableSetupColumn("ry");
        ImGui::TableHeadersRow();
        for (auto& w : g_map.waypoints) {
            ImGui::TableNextRow();
            bool sel = w.id == g_view.selectedWp;
            ImGui::TableNextColumn();
            if (ImGui::Selectable(std::to_string(w.id).c_str(), sel,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                g_view.selectedWp = w.id;
            }
            ImGui::TableNextColumn(); ImGui::Text("%d", w.rx);
            ImGui::TableNextColumn(); ImGui::Text("%d", w.ry);
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    RenderViewport(avail.x, avail.y);

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
    // headless mode: map-tool.exe --dump <file> -> prints stats, exit 0/2
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

    LoadConfig();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
                       GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr,
                       nullptr, nullptr };
    wc.lpszClassName = L"Vera20kMapTool";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"VERA20K 地图编辑器",
                                WS_OVERLAPPEDWINDOW, 60, 40, 1500, 920,
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

    // optional: load a map from the command line
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc >= 2) {
            std::string data = LoadFileUtf8(argv[1]);
            std::string err;
            if (g_map.loadFromData(data, &err)) {
                g_loaded = true;
                int wl = lstrlenW(argv[1]);
                int nl = WideCharToMultiByte(CP_UTF8, 0, argv[1], wl, nullptr, 0, nullptr, nullptr);
                g_map.path.resize(nl);
                WideCharToMultiByte(CP_UTF8, 0, argv[1], wl, g_map.path.data(), nl, nullptr, nullptr);
                ReleaseTheater();
                EnsureTheater();
                PrepareMapView();
                char buf[512];
                snprintf(buf, sizeof(buf), "已加载：%s -- %d 格，%d 个路径点%s%s",
                         g_map.name.c_str(), (int)g_map.cells.size(), (int)g_map.waypoints.size(),
                         g_theater ? " -- " : "",
                         g_theater ? g_theater->theater().c_str() : (g_theaterError.empty() ? "" : g_theaterError.c_str()));
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
