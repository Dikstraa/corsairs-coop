// Names over other players' ships on the world map and at sea, and the player list (F3).
//
// The engine gives a module no way to draw, so the game's Direct3D 9 device is used directly: its
// EndScene (and Reset, for the font) is redirected through the device's table of methods, which every
// device of d3d9.dll shares, found by making a throwaway device on a hidden window. In EndScene the
// ships' positions are projected with the camera the engine set (SetTransform), and each name is
// drawn just over its ship with d3dx9_43.dll's font (shipped with the game), white over a shadow.

#include "map_labels.h"

#include <d3d9.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace coopnet
{
void LogLine(const char *line); // coopnet.cpp
}

void Log(const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    coopnet::LogLine(buf);
}

namespace
{
// ID3DXFont, as d3dx9core.h declares it (only DrawTextW and the device calls are used).
struct ID3DXFontLike : IUnknown
{
    virtual HRESULT __stdcall GetDevice(IDirect3DDevice9 **) = 0;
    virtual HRESULT __stdcall GetDescA(void *) = 0;
    virtual HRESULT __stdcall GetDescW(void *) = 0;
    virtual BOOL __stdcall GetTextMetricsA(void *) = 0;
    virtual BOOL __stdcall GetTextMetricsW(void *) = 0;
    virtual HDC __stdcall GetDC() = 0;
    virtual HRESULT __stdcall GetGlyphData(UINT, void **, RECT *, POINT *) = 0;
    virtual HRESULT __stdcall PreloadCharacters(UINT, UINT) = 0;
    virtual HRESULT __stdcall PreloadGlyphs(UINT, UINT) = 0;
    virtual HRESULT __stdcall PreloadTextA(LPCSTR, INT) = 0;
    virtual HRESULT __stdcall PreloadTextW(LPCWSTR, INT) = 0;
    virtual INT __stdcall DrawTextA(void *, LPCSTR, INT, RECT *, DWORD, D3DCOLOR) = 0;
    virtual INT __stdcall DrawTextW(void *, LPCWSTR, INT, RECT *, DWORD, D3DCOLOR) = 0;
    virtual HRESULT __stdcall OnLostDevice() = 0;
    virtual HRESULT __stdcall OnResetDevice() = 0;
};

using CreateFontW_t = HRESULT(WINAPI *)(IDirect3DDevice9 *, INT, UINT, UINT, UINT, BOOL, DWORD, DWORD, DWORD, DWORD,
                                        LPCWSTR, ID3DXFontLike **);
using EndScene_t = HRESULT(__stdcall *)(IDirect3DDevice9 *);
using Reset_t = HRESULT(__stdcall *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);

constexpr int kSlotReset = 16;
constexpr int kSlotEndScene = 42;
constexpr int kSlotSetTransform = 44;

constexpr int kMaxTables = 8;
void **g_table[kMaxTables] = {}; // the device method tables patched, and what was there
EndScene_t g_origEndScene[kMaxTables] = {};
Reset_t g_origReset[kMaxTables] = {};
using SetTransform_t = HRESULT(__stdcall *)(IDirect3DDevice9 *, D3DTRANSFORMSTATETYPE, const D3DMATRIX *);
SetTransform_t g_origSetTransform[kMaxTables] = {};

// The camera the map was drawn with. By EndScene the interface has set its flat one, and the map is
// drawn around the camera (its view is only turned, objects carry "position - camera"), so the
// player's own ship is found among the objects of a frame and the others are placed from it (their
// position - the player's + that object's). The camera follows the ship at a fixed offset, so once
// found the ship stays where it was drawn: each frame takes the object nearest to that. Only to find it
// first (or again after a zoom moved it for a while) the object nearest the middle of the view is
// taken - which may be anything along the line of sight, an island far ahead too: that one put every
// name hundreds of units off, jumping about as the view changed.
D3DMATRIX g_view = {}, g_proj = {};
bool g_projIs3d = false;
float g_frameOff = 1e9f, g_frameDepth = 1e9f, g_frameCenter[3] = {}; // this frame: the ship as found anew
D3DMATRIX g_frameView = {}, g_frameProj = {};
float g_trackDist = 1e9f, g_trackCenter[3] = {};	// this frame: the object nearest to where the ship was
D3DMATRIX g_trackView = {}, g_trackProj = {};
bool g_haveCam = false;
float g_camCenter[3] = {};
D3DMATRIX g_camView = {}, g_camProj = {};
DWORD g_trackedAt = 0;	  // GetTickCount when the ship was last found where it was
int g_reacquired = 0;	  // for the log
float g_meX = 0, g_meY = 0, g_meZ = 0;
// The sea is drawn around the camera the same way, so it is done the same way (with heights).
bool g_sea = false;
constexpr float kTrackNear = 10.0f;	// map units the ship may move from one frame to the next (zoom, waves)
constexpr DWORD kTrackLost = 1500;	// ms without it before it is looked for anew
int g_tables = 0;

float Det(const D3DMATRIX &v)
{
    return v._11 * (v._22 * v._33 - v._23 * v._32) - v._12 * (v._21 * v._33 - v._23 * v._31) +
           v._13 * (v._21 * v._32 - v._22 * v._31);
}

int TableOf(IDirect3DDevice9 *dev)
{
    void **table = *reinterpret_cast<void ***>(dev);
    for (int i = 0; i < g_tables; ++i)
        if (g_table[i] == table)
            return i;
    return 0;
}
ID3DXFontLike *g_font = nullptr;
IDirect3DDevice9 *g_fontDevice = nullptr;
bool g_fontFailed = false;

std::mutex g_mu;
std::vector<coopnet::MapLabel> g_labels;
DWORD g_labelsAt = 0; // GetTickCount when the list was last given: an old list (the map is gone) is not drawn

void MakeFont(IDirect3DDevice9 *dev)
{
    if (g_font || g_fontFailed)
        return;
    HMODULE d3dx = LoadLibraryA("d3dx9_43.dll");
    auto create = d3dx ? reinterpret_cast<CreateFontW_t>(GetProcAddress(d3dx, "D3DXCreateFontW")) : nullptr;
    if (!create || FAILED(create(dev, 18, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial", &g_font)))
    {
        g_fontFailed = true;
        g_font = nullptr;
        Log("map labels: no font (d3dx9_43.dll)");
        return;
    }
    g_fontDevice = dev;
    Log("map labels: font ready");
}

std::wstring Wide(const std::string &text)
{
    int n = MultiByteToWideChar(1251, 0, text.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n : 1, L'\0');
    if (n > 0)
        MultiByteToWideChar(1251, 0, text.c_str(), -1, &w[0], n);
    w.resize(wcslen(w.c_str()));
    return w;
}

// The player list: F3 shows and hides it (read here, so it works on every screen of the game).
std::string g_panel;
DWORD g_panelAt = 0;
bool g_panelOn = false, g_panelKey = false;

bool GameActive()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void DrawPanel(IDirect3DDevice9 *dev)
{
    static bool testOn = false;
    if (!testOn)
    {
        testOn = true;
        char flag[4] = {};
        if (GetEnvironmentVariableA("COOPNET_PANELTEST", flag, sizeof flag) > 0 && flag[0] == '1')
            g_panelOn = true; // test runs: shown from the start
    }
    bool down = (GetAsyncKeyState(VK_F3) & 0x8000) != 0 && GameActive();
    if (down && !g_panelKey)
    {
        g_panelOn = !g_panelOn;
        Log("player list %s", g_panelOn ? "shown" : "hidden");
    }
    g_panelKey = down;
    if (!g_panelOn)
        return;
    std::string text;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (GetTickCount() - g_panelAt < 3000)
            text = g_panel;
    }
    std::vector<std::wstring> lines;
    size_t from = 0;
    while (!text.empty() && from <= text.size())
    {
        size_t bar = text.find('|', from);
        if (bar == std::string::npos)
            bar = text.size();
        lines.push_back(Wide(text.substr(from, bar - from)));
        from = bar + 1;
    }
    if (lines.empty()) // "Кооператив: нет игры по сети"
        lines.push_back(L"Кооператив: нет игры "
                        L"по сети");
    MakeFont(dev);
    D3DVIEWPORT9 vp;
    if (!g_font || dev != g_fontDevice || FAILED(dev->GetViewport(&vp)))
        return;
    LONG width = 300;
    for (const auto &l : lines)
    {
        RECT r = {0, 0, 0, 0};
        g_font->DrawTextW(nullptr, l.c_str(), -1, &r, DT_CALCRECT | DT_LEFT | DT_SINGLELINE, 0);
        if (r.right + 40 > width)
            width = r.right + 40;
    }
    const LONG row = 24;
    LONG height = static_cast<LONG>(lines.size()) * row + 24;
    LONG left = static_cast<LONG>(vp.X) + (static_cast<LONG>(vp.Width) - width) / 2;
    LONG top = static_cast<LONG>(vp.Y + vp.Height / 6);
    D3DRECT frame = {left - 2, top - 2, left + width + 2, top + height + 2};
    D3DRECT box = {left, top, left + width, top + height};
    dev->Clear(1, &frame, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 170, 140, 70), 1.0f, 0);
    dev->Clear(1, &box, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 22, 26, 34), 1.0f, 0);
    for (size_t i = 0; i < lines.size(); ++i)
    {
        LONG y = top + 12 + static_cast<LONG>(i) * row;
        RECT r = {left + 20, y, left + width - 20, y + row};
        D3DCOLOR color = i == 0 ? D3DCOLOR_ARGB(255, 240, 200, 110) : D3DCOLOR_ARGB(255, 235, 235, 235);
        g_font->DrawTextW(nullptr, lines[i].c_str(), -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP, color);
    }
}

void DrawLabels(IDirect3DDevice9 *dev)
{
    std::vector<coopnet::MapLabel> labels;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_labels.empty() || GetTickCount() - g_labelsAt > 300)
            return;
        labels = g_labels;
    }
    MakeFont(dev);
    if (!g_font || dev != g_fontDevice)
        return;
    static DWORD noCameraAt = 0;
    if (!g_haveCam && GetTickCount() - noCameraAt > 60000)
    {
        noCameraAt = GetTickCount();
        Log("map labels: no map camera seen yet");
    }
    D3DVIEWPORT9 vp;
    if (!g_haveCam || FAILED(dev->GetViewport(&vp)))
        return;
    const D3DMATRIX &view = g_camView;
    const D3DMATRIX &proj = g_camProj;
    for (const auto &l : labels)
    {
        // where the camera draws it: its offset from the player's ship + the ship's drawn position
        float px = l.x - g_meX + g_camCenter[0];
        float py = l.y - g_meY + g_camCenter[1];
        float pz = l.z - g_meZ + g_camCenter[2];
        // view -> clip (row vectors, as Direct3D's fixed pipeline)
        float vx = px * view._11 + py * view._21 + pz * view._31 + view._41;
        float vy = px * view._12 + py * view._22 + pz * view._32 + view._42;
        float vz = px * view._13 + py * view._23 + pz * view._33 + view._43;
        float cx = vx * proj._11 + vy * proj._21 + vz * proj._31 + proj._41;
        float cy = vx * proj._12 + vy * proj._22 + vz * proj._32 + proj._42;
        float cw = vx * proj._14 + vy * proj._24 + vz * proj._34 + proj._44;
        std::wstring w = Wide(l.text);
        const float margin = 60.0f;
        float halfW = vp.Width * 0.5f, halfH = vp.Height * 0.5f;
        float sx = 0, sy = 0;
        bool onScreen = false;
        if (cw > 0.01f)
        {
            sx = vp.X + (cx / cw + 1.0f) * halfW;
            sy = vp.Y + (1.0f - cy / cw) * halfH;
            onScreen = sx > vp.X + margin && sx < vp.X + vp.Width - margin && sy > vp.Y + margin &&
                       sy < vp.Y + vp.Height - margin;
        }
        LONG top = -30; // over the ship
        if (!onScreen)
        {
            // off the screen: at its edge, toward the ship, with an arrow
            float dx = cx, dy = -cy;
            if (cw <= 0.01f)
            {
                // behind the camera the projection turns over: as on a chart instead, ahead up, behind
                // down, left and right as they are
                dx = vx;
                dy = -vz;
            }
            float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-6f)
                continue;
            dx /= len;
            dy /= len;
            float tx = std::fabs(dx) > 1e-6f ? (halfW - margin) / std::fabs(dx) : 1e9f;
            float ty = std::fabs(dy) > 1e-6f ? (halfH - margin) / std::fabs(dy) : 1e9f;
            float t = tx < ty ? tx : ty;
            sx = vp.X + halfW + dx * t;
            sy = vp.Y + halfH + dy * t;
            const wchar_t *arrow = std::fabs(dx) > std::fabs(dy) ? (dx > 0 ? L" \x2192" : L"\x2190 ") : (dy > 0 ? L" \x2193" : L"\x2191 ");
            w = (dx > 0 && std::fabs(dx) > std::fabs(dy)) || (dy > 0 && std::fabs(dy) >= std::fabs(dx)) ? w + arrow : arrow + w;
            top = -10;
        }
        RECT r = {static_cast<LONG>(sx) - 200, static_cast<LONG>(sy) + top, static_cast<LONG>(sx) + 200,
                  static_cast<LONG>(sy) + top + 26};
        RECT shadow = {r.left + 1, r.top + 1, r.right + 1, r.bottom + 1};
        g_font->DrawTextW(nullptr, w.c_str(), -1, &shadow, DT_CENTER | DT_TOP | DT_NOCLIP, D3DCOLOR_ARGB(200, 0, 0, 0));
        g_font->DrawTextW(nullptr, w.c_str(), -1, &r, DT_CENTER | DT_TOP | DT_NOCLIP, D3DCOLOR_ARGB(255, 255, 240, 200));
        static DWORD loggedAt = 0;
        static bool test = GetEnvironmentVariableA("COOPNET_TEST", nullptr, 0) > 0;
        if (GetTickCount() - loggedAt > (test ? 1000u : 60000u))
        {
            loggedAt = GetTickCount();
            Log("map labels: \"%s\" at %.1f %.1f %s (%ux%u); ship %.1f %.1f, me %.1f %.1f, own ship drawn at %.1f %.1f %.1f, "
                "found anew %d times",
                l.text.c_str(), sx, sy, onScreen ? "on screen" : "at the edge", vp.Width, vp.Height, l.x, l.z, g_meX, g_meZ,
                g_camCenter[0], g_camCenter[1], g_camCenter[2], g_reacquired);
        }
    }
}

HRESULT __stdcall HookEndScene(IDirect3DDevice9 *dev)
{
    static bool seen = false;
    if (!seen)
    {
        seen = true;
        Log("map labels: the game draws through the hook");
    }
    DrawLabels(dev);
    DrawPanel(dev);
    // the frame drawn: its camera is the one for the next labels
    if (g_haveCam && g_trackDist < kTrackNear)
    {
        g_camView = g_trackView;
        g_camProj = g_trackProj;
        g_camCenter[0] = g_trackCenter[0];
        g_camCenter[1] = g_trackCenter[1];
        g_camCenter[2] = g_trackCenter[2];
        g_trackedAt = GetTickCount();
    }
    else if (g_frameOff < 0.2f && (!g_haveCam || GetTickCount() - g_trackedAt > kTrackLost))
    {
        if (g_haveCam)
            ++g_reacquired;
        g_haveCam = true;
        g_camView = g_frameView;
        g_camProj = g_frameProj;
        g_camCenter[0] = g_frameCenter[0];
        g_camCenter[1] = g_frameCenter[1];
        g_camCenter[2] = g_frameCenter[2];
        g_trackedAt = GetTickCount();
    }
    g_frameOff = 1e9f;
    g_frameDepth = 1e9f;
    g_trackDist = 1e9f;
    return g_origEndScene[TableOf(dev)](dev);
}

HRESULT __stdcall HookSetTransform(IDirect3DDevice9 *dev, D3DTRANSFORMSTATETYPE state, const D3DMATRIX *m)
{
    if (m && state == D3DTS_VIEW)
        g_view = *m;
    if (m && state == D3DTS_PROJECTION)
    {
        // a perspective projection has w = z: _34 = 1, _44 = 0; the interface draws with one too, but
        // scaled by 2 / screen width, a camera by its field of view (about 1)
        g_projIs3d = std::fabs(m->_34 - 1.0f) < 0.001f && std::fabs(m->_44) < 0.001f && std::fabs(m->_11) > 0.1f;
        if (g_projIs3d)
            g_proj = *m;
    }
    // a mirrored view draws the reflection in the water: left out
    if (m && state == D3DTS_WORLD && g_projIs3d && Det(g_view) > 0.0f)
    {
        // the object nearest the middle of the view this frame: the camera looks at the player's ship
        const D3DMATRIX &v = g_view;
        float x = m->_41 * v._11 + m->_42 * v._21 + m->_43 * v._31 + v._41;
        float y = m->_41 * v._12 + m->_42 * v._22 + m->_43 * v._32 + v._42;
        float z = m->_41 * v._13 + m->_42 * v._23 + m->_43 * v._33 + v._43;
        if (z > 1.0f && g_haveCam)
        {
            float dx = m->_41 - g_camCenter[0], dy = m->_42 - g_camCenter[1], dz = m->_43 - g_camCenter[2];
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist < g_trackDist)
            {
                g_trackDist = dist;
                g_trackCenter[0] = m->_41;
                g_trackCenter[1] = m->_42;
                g_trackCenter[2] = m->_43;
                g_trackView = v;
                g_trackProj = g_proj;
            }
        }
        if (z > 1.0f)
        {
            // near the middle of the view (off < 0.2), the nearest to the camera: the ship, not an island
            // far along the line of sight
            float off = (std::fabs(x) + std::fabs(y)) / z;
            bool better = off < 0.2f ? (g_frameOff >= 0.2f || z < g_frameDepth) : off < g_frameOff;
            if (better)
            {
                g_frameOff = off;
                g_frameDepth = z;
                g_frameCenter[0] = m->_41;
                g_frameCenter[1] = m->_42;
                g_frameCenter[2] = m->_43;
                g_frameView = v;
                g_frameProj = g_proj;
            }
        }
    }
    return g_origSetTransform[TableOf(dev)](dev, state, m);
}

HRESULT __stdcall HookReset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    if (g_font)
        g_font->OnLostDevice();
    HRESULT hr = g_origReset[TableOf(dev)](dev, pp);
    if (g_font && SUCCEEDED(hr))
        g_font->OnResetDevice();
    return hr;
}

// Device tables in one memory region: the same EndScene and the calls around it as mine (not mine).
int FindTables(void **from, size_t count, void **mine, void *endScene, void *beginScene, void *clear, void *setTransform,
               void ***found, int max)
{
    int n = 0;
    __try
    {
        for (size_t i = kSlotEndScene; i + 2 < count && n < max; ++i)
        {
            if (from[i] != endScene)
                continue;
            void **table = from + i - kSlotEndScene;
            if (table != mine && table[41] == beginScene && table[43] == clear && table[44] == setTransform)
                found[n++] = table;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return n;
}

void Patch(void **table, int slot, void *hook, void **orig)
{
    DWORD old;
    if (!VirtualProtect(&table[slot], sizeof(void *), PAGE_EXECUTE_READWRITE, &old))
        return;
    *orig = table[slot];
    table[slot] = hook;
    VirtualProtect(&table[slot], sizeof(void *), old, &old);
}
} // namespace

namespace coopnet
{
bool HookMapLabels()
{
    static bool tried = false;
    if (tried)
        return g_tables > 0;
    tried = true;
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    auto create = d3d9 ? reinterpret_cast<IDirect3D9 *(WINAPI *)(UINT)>(GetProcAddress(d3d9, "Direct3DCreate9")) : nullptr;
    if (!create)
    {
        Log("map labels: d3d9.dll not loaded");
        return false;
    }
    IDirect3D9 *d3d = create(D3D_SDK_VERSION);
    if (!d3d)
        return false;
    HWND wnd = CreateWindowExA(0, "STATIC", "coopnet", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.BackBufferCount = 1;
    IDirect3DDevice9 *dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr) || !dev)
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr) || !dev)
    {
        Log("map labels: no throwaway device (0x%08lx)", static_cast<unsigned long>(hr));
        d3d->Release();
        DestroyWindow(wnd);
        return false;
    }
    // Every device here has a table of its own, but they all point at the same functions: the game's
    // device table is any other table with these entries (Reset and Present may be redirected by the
    // Steam overlay, so the plain drawing calls around EndScene are compared).
    void **mine = *reinterpret_cast<void ***>(dev);
    void *endScene = mine[kSlotEndScene];
    void *beginScene = mine[41];
    void *clear = mine[43];
    void *setTransform = mine[44];
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    auto addr = static_cast<BYTE *>(si.lpMinimumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi;
    int hits = 0;
    while (addr < si.lpMaximumApplicationAddress && VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi)
    {
        DWORD prot = mbi.Protect & 0xff;
        bool readable = prot == PAGE_READWRITE || prot == PAGE_READONLY || prot == PAGE_WRITECOPY ||
                        prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_READ;
        if (mbi.State == MEM_COMMIT && readable && !(mbi.Protect & PAGE_GUARD))
        {
            void **found[kMaxTables];
            int n = FindTables(static_cast<void **>(mbi.BaseAddress), mbi.RegionSize / sizeof(void *), mine, endScene,
                               beginScene, clear, setTransform, found, kMaxTables - g_tables);
            for (int f = 0; f < n; ++f)
            {
                void **table = found[f];
                ++hits;
                g_table[g_tables] = table;
                Patch(table, kSlotEndScene, reinterpret_cast<void *>(HookEndScene),
                      reinterpret_cast<void **>(&g_origEndScene[g_tables]));
                Patch(table, kSlotReset, reinterpret_cast<void *>(HookReset), reinterpret_cast<void **>(&g_origReset[g_tables]));
                Patch(table, kSlotSetTransform, reinterpret_cast<void *>(HookSetTransform),
                      reinterpret_cast<void **>(&g_origSetTransform[g_tables]));
                ++g_tables;
            }
        }
        addr = static_cast<BYTE *>(mbi.BaseAddress) + mbi.RegionSize;
    }
    dev->Release();
    d3d->Release();
    DestroyWindow(wnd);
    Log("map labels: hooked EndScene of %d device table(s)", hits);
    return g_tables > 0;
}

void SetPanel(const std::string &text)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_panel = text;
    g_panelAt = GetTickCount();
}

void SetLabels(const std::vector<MapLabel> &labels, float meX, float meY, float meZ, bool sea)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_labels = labels;
    g_labelsAt = GetTickCount();
    g_meX = meX;
    g_meY = meY;
    g_meZ = meZ;
    if (sea != g_sea)
    {
        g_sea = sea;
        g_haveCam = false; // the other scene's camera
    }
}
} // namespace coopnet
