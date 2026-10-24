// CoopNet: native script library (#library "COOPNET") exposing the co-op network session.
//
// Scripts build an outgoing message with CoopNet_Begin/Put*/Send and read incoming ones with
// CoopNet_Recv/From/Get*. Field encoding and system message types: see wire.h and session.h.
#include "coop_clock.h"
#include "coop_vote.h"
#include "map_labels.h"
#include "maelstrom_abi.h"
#include "session.h"
#include "wire.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <deque>
#include <map>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace mael;

namespace
{

constexpr long kVersion = 10;

// Message types 10..99 are handled inside the DLL and never reach scripts.
enum : uint16_t
{
    kMsgClock = 10,       // host -> all:    f64 gameSeconds, f64 rate, u32 epoch
    kMsgPlayerTime = 11,  // client -> host: i32 mode, u8 battle, f64 rate
    kMsgSkipRequest = 12, // client -> host: u32 tag, f64 seconds
    kMsgSkipVote = 13,    // host -> all:    u32 voteId, i32 requester, str requesterName, f64 seconds
    kMsgSkipAnswer = 14,  // client -> host: u32 voteId, u8 yes
    kMsgSkipResult = 15,  // host -> all:    u32 voteId, i32 requester, u32 tag, u8 approved, f64 seconds
    kFirstInternal = 10,
    kLastInternal = 99,
};

// Events the DLL itself raises for scripts (next to the session's 1..5).
enum : uint16_t
{
    kEvtSkipVote = 6,   // string text: somebody asks to skip time, F8 agrees
    kEvtSkipResult = 7, // int approved, int mine, string continuation, string text
    kEvtNotice = 8,     // string text
};
constexpr long kClockSourceWorldMap = 1; // CoopNet_ClockEngine sources, see coopnet.c
constexpr double kClockBroadcastEvery = 0.25;
constexpr double kPlayerTimeEvery = 0.5;

VAPI *g_api = nullptr;
std::string g_lastError;

coopnet::Writer g_out;   // message being built by CoopNet_Begin/Put*
coopnet::Message g_in;   // message returned by the last CoopNet_Recv
coopnet::Reader g_inRd;  // read position inside g_in
std::string g_retString; // keeps returned strings alive after the native returns

coopnet::Clock g_clock;
coopnet::RateMeter g_meter;
coopnet::PlayerTime g_local;   // this player's mode/battle (from the script tick) and pace
double g_fixedRate = 0;        // pace reported by scripts for the current mode (world map), 0 = measured
coopnet::GameDate g_applied;   // date returned by the last CoopNet_ClockEngine
int g_appliedDaysAhead = 0;    // whole days g_applied is ahead of the engine's date
uint32_t g_hostEpoch = 0;      // host: clock restarts so far

coopnet::VoteBook g_votes;                        // host: the running time-skip vote
uint32_t g_voteToAnswer = 0;                      // vote this player was asked about, 0 = none
uint32_t g_nextSkipTag = 0;                       // this player's skip requests
std::map<uint32_t, std::string> g_skipContinue;   // tag -> quest to complete afterwards
std::deque<coopnet::Message> g_localEvents;       // raised by the DLL, returned before network ones
double g_lastUpdate = -1;      // real time of the last CoopNet_Update
double g_lastClockSend = -1;
double g_lastPlayerTimeSend = -1;

double RealNow()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Sessions are created on first use and intentionally never destroyed: joining the network thread
// from a static destructor would run under the loader lock.
coopnet::Session *g_udp = nullptr;
coopnet::Session *g_steam = nullptr;
coopnet::Session *g_active = nullptr;

coopnet::Session &Net()
{
    if (!g_active)
        g_active = g_udp = coopnet::CreateUdpSession().release();
    return *g_active;
}

void Log(const char *fmt, ...)
{
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    if (FILE *f = fopen("coopnet.log", "a"))
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "[%02d:%02d:%02d.%03d pid %lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                GetCurrentProcessId());
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fputc('\n', f);
        fclose(f);
    }
}

std::string ArgString(VDATA *d)
{
    const char *s = d ? GetString(d) : nullptr;
    return s ? s : "";
}

DWORD Return(VS_STACK *s, long v)
{
    VDATA *r = Push(s);
    if (!r)
        return IFUNCRESULT_FAILED;
    Set(r, v);
    return IFUNCRESULT_OK;
}

DWORD Return(VS_STACK *s, float v)
{
    VDATA *r = Push(s);
    if (!r)
        return IFUNCRESULT_FAILED;
    Set(r, v);
    return IFUNCRESULT_OK;
}

DWORD Return(VS_STACK *s, const std::string &v)
{
    VDATA *r = Push(s);
    if (!r)
        return IFUNCRESULT_FAILED;
    g_retString = v;
    Set(r, g_retString.c_str());
    return IFUNCRESULT_OK;
}

// ---- session control ---------------------------------------------------------

// int CoopNet_Host(int port, int maxPeers, string name)
DWORD __cdecl N_Host(VS_STACK *s)
{
    std::string name = ArgString(Pop(s));
    long maxPeers = GetLong(Pop(s));
    long port = GetLong(Pop(s));
    bool ok = Net().Host(static_cast<uint16_t>(port), maxPeers, name, g_lastError);
    if (ok)
        Log("hosting on port %ld, max %ld peers, as '%s'", port, maxPeers, name.c_str());
    else
        Log("host on port %ld failed: %s", port, g_lastError.c_str());
    return Return(s, ok ? 1L : 0L);
}

// int CoopNet_Connect(string host, int port, string name)
DWORD __cdecl N_Connect(VS_STACK *s)
{
    std::string name = ArgString(Pop(s));
    long port = GetLong(Pop(s));
    std::string host = ArgString(Pop(s));
    bool ok = Net().Connect(host, static_cast<uint16_t>(port), name, g_lastError);
    if (ok)
        Log("connecting to %s:%ld as '%s'", host.c_str(), port, name.c_str());
    else
        Log("connect to %s:%ld failed: %s", host.c_str(), port, g_lastError.c_str());
    return Return(s, ok ? 1L : 0L);
}

// int CoopNet_UseSteam()  switch to the Steam (Spacewar) transport; 0 if Steam isn't available
DWORD __cdecl N_UseSteam(VS_STACK *s)
{
    if (!g_steam)
    {
        auto steam = coopnet::CreateSteamSession(g_lastError);
        if (!steam)
        {
            Log("steam unavailable: %s", g_lastError.c_str());
            return Return(s, 0L);
        }
        g_steam = steam.release();
        Log("steam: ready");
    }
    if (g_active != g_steam)
    {
        Net().Disconnect();
        g_active = g_steam;
    }
    return Return(s, 1L);
}

// int CoopNet_Invite()  opens the Steam overlay's invite dialog (or friends list); 0 without Steam
DWORD __cdecl N_Invite(VS_STACK *s)
{
    return Return(s, static_cast<long>(g_active == g_steam && g_steam && g_steam->OpenInviteDialog()));
}

// string CoopNet_MyUid()  this player's id across sessions ("steam<SteamID64>", usable as an attribute name), "" without Steam
DWORD __cdecl N_MyUid(VS_STACK *s)
{
    return Return(s, Net().LocalUid());
}

// The saves a rollback may use: written since this game was started, or the one the host loaded.
FILETIME g_launchTime = {};
std::string g_loadedSave;

// string CoopNet_RollbackSave(string dir)  the newer of: the latest save in dir written since the game
// was started, the save loaded last (CoopNet_SaveLoaded); "" when there is neither
DWORD __cdecl N_RollbackSave(VS_STACK *s)
{
    std::string dir = ArgString(Pop(s));
    if (!dir.empty() && dir.back() != '\\' && dir.back() != '/')
        dir += '\\';
    std::string best;
    FILETIME bestTime = g_launchTime;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0)
            {
                best = dir + fd.cFileName;
                bestTime = fd.ftLastWriteTime;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (best.empty())
        best = g_loadedSave;
    return Return(s, best);
}

// int CoopNet_SaveLoaded(string path)  from LoadGame: the save the game was loaded from (kept here, as a
// load replaces the script's own variables)
DWORD __cdecl N_SaveLoaded(VS_STACK *s)
{
    g_loadedSave = ArgString(Pop(s));
    Log("save loaded: %s", g_loadedSave.c_str());
    return Return(s, 1L);
}
// int CoopNet_UseUdp()  switch to the direct UDP transport (LAN, VPN, forwarded port)
DWORD __cdecl N_UseUdp(VS_STACK *s)
{
    if (g_active && g_active != g_udp)
    {
        Net().Disconnect();
        g_active = nullptr; // Net() recreates/reuses UDP
        if (g_udp)
            g_active = g_udp;
    }
    return Return(s, 1L);
}

// string CoopNet_Config(string key)  value from [coop] in coop.ini next to Maelstrom.exe, "" if absent
DWORD __cdecl N_Config(VS_STACK *s)
{
    std::string key = ArgString(Pop(s));
    char buf[256] = {};
    GetPrivateProfileStringA("coop", key.c_str(), "", buf, sizeof buf, ".\\coop.ini");
    return Return(s, std::string(buf));
}

// int CoopNet_Disconnect()
DWORD __cdecl N_Disconnect(VS_STACK *s)
{
    Net().Disconnect();
    Log("disconnected");
    return Return(s, 1L);
}

// int CoopNet_State()  0 offline, 1 connecting, 2 connected, 3 hosting
DWORD __cdecl N_State(VS_STACK *s) { return Return(s, static_cast<long>(Net().GetState())); }

// int CoopNet_MyId()  host 0, clients 1.., -1 offline
DWORD __cdecl N_MyId(VS_STACK *s) { return Return(s, static_cast<long>(Net().MyId())); }

// int CoopNet_PeerCount()
DWORD __cdecl N_PeerCount(VS_STACK *s) { return Return(s, static_cast<long>(Net().Peers().size())); }

// int CoopNet_PeerId(int index)  -1 if out of range
DWORD __cdecl N_PeerId(VS_STACK *s)
{
    long index = GetLong(Pop(s));
    auto peers = Net().Peers();
    return Return(s, index >= 0 && index < static_cast<long>(peers.size()) ? static_cast<long>(peers[index]) : -1L);
}

// string CoopNet_PeerName(int id)
DWORD __cdecl N_PeerName(VS_STACK *s)
{
    long id = GetLong(Pop(s));
    return Return(s, Net().PeerName(id));
}

// int CoopNet_Ping(int id)  round trip in ms, -1 unknown
DWORD __cdecl N_Ping(VS_STACK *s)
{
    long id = GetLong(Pop(s));
    return Return(s, static_cast<long>(Net().PingMs(id)));
}

// string CoopNet_LastError()
DWORD __cdecl N_LastError(VS_STACK *s) { return Return(s, g_lastError); }

// int CoopNet_SetLoss(float fraction)  debug: drop part of outgoing packets
DWORD __cdecl N_SetLoss(VS_STACK *s)
{
    float fraction = GetFloat(Pop(s));
    Net().SetSimulatedLoss(fraction);
    return Return(s, 1L);
}

// ---- running in the background -----------------------------------------------
// The engine stops running frames when its window is deactivated; it only learns that from
// WM_ACTIVATE/WM_ACTIVATEAPP. During a co-op session the world must keep going for the others,
// so those notifications are hidden from it.

WNDPROC g_origWndProc = nullptr;

LRESULT CALLBACK CoopWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (Net().GetState() != coopnet::State::Offline)
    {
        bool away = (msg == WM_ACTIVATEAPP && wp == FALSE) || (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE);
        if (away)
        {
            // the engine isn't told, so the mouse it holds is let go here
            ClipCursor(nullptr);
            ReleaseCapture();
            return 0;
        }
    }
    return CallWindowProcA(g_origWndProc, hwnd, msg, wp, lp);
}

// The engine believes it is still active, so it keeps grabbing the mouse: it captures and clips the
// cursor (Maelstrom.exe) and puts it back in the middle every frame (PCs_Controls.dll, GameUtils.dll,
// XInterface.dll). Their imports of those calls are redirected here, where they do nothing while
// another window is in front.
HWND g_gameWindow = nullptr;

bool GameInBackground()
{
    return g_gameWindow && GetForegroundWindow() != g_gameWindow;
}

long g_mouseHeldBack = 0; // calls kept from moving the mouse while the game is behind

BOOL WINAPI HookSetCursorPos(int x, int y)
{
    if (!GameInBackground())
        return SetCursorPos(x, y);
    ++g_mouseHeldBack;
    return TRUE;
}

BOOL WINAPI HookClipCursor(const RECT *rect)
{
    return ClipCursor(GameInBackground() ? nullptr : rect);
}

HWND WINAPI HookSetCapture(HWND hwnd)
{
    return GameInBackground() ? nullptr : SetCapture(hwnd);
}

// Points module's import of user32!name at hook.
void HookImport(HMODULE module, const char *name, void *hook)
{
    auto base = reinterpret_cast<BYTE *>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return;
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); imp->Name; ++imp)
    {
        if (_stricmp(reinterpret_cast<char *>(base + imp->Name), "user32.dll") != 0 || !imp->OriginalFirstThunk)
            continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots)
        {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (strcmp(byName->Name, name) != 0)
                continue;
            DWORD old;
            if (VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old))
            {
                slots->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
                VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
            }
        }
    }
}

// Test runs (COOPNET_TEST set): keys a script holds down with CoopNet_TestKey, as the controls read them
// (PCs_Controls.dll polls GetAsyncKeyState), so a test can sail the world map without the keyboard.
bool g_testKeys[256] = {};

SHORT WINAPI HookGetAsyncKeyState(int vk)
{
    if (vk >= 0 && vk < 256 && g_testKeys[vk])
        return static_cast<SHORT>(0x8000);
    return GetAsyncKeyState(vk);
}

SHORT WINAPI HookGetKeyState(int vk)
{
    if (vk >= 0 && vk < 256 && g_testKeys[vk])
        return static_cast<SHORT>(0x8000);
    return GetKeyState(vk);
}

void ReleaseMouseInBackground(HWND hwnd)
{
    g_gameWindow = hwnd;
    for (const char *dll : {(const char *)nullptr, "PCs_Controls.dll", "GameUtils.dll", "XInterface.dll"})
    {
        HMODULE module = dll ? GetModuleHandleA(dll) : GetModuleHandleA(nullptr);
        if (!module)
            continue;
        HookImport(module, "SetCursorPos", reinterpret_cast<void *>(HookSetCursorPos));
        HookImport(module, "ClipCursor", reinterpret_cast<void *>(HookClipCursor));
        HookImport(module, "SetCapture", reinterpret_cast<void *>(HookSetCapture));
        if (GetEnvironmentVariableA("COOPNET_TEST", nullptr, 0) > 0)
        {
            HookImport(module, "GetAsyncKeyState", reinterpret_cast<void *>(HookGetAsyncKeyState));
            HookImport(module, "GetKeyState", reinterpret_cast<void *>(HookGetKeyState));
        }
    }
}

BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM out)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    char title[64] = {};
    GetWindowTextA(hwnd, title, sizeof title);
    if (pid == GetCurrentProcessId() && strncmp(title, "Pirates of the Maelstro", 23) == 0)
    {
        *reinterpret_cast<HWND *>(out) = hwnd;
        return FALSE;
    }
    return TRUE;
}

void KeepRunningInBackground()
{
    if (g_origWndProc)
        return;
    HWND hwnd = nullptr;
    EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&hwnd));
    if (!hwnd)
        return;
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(CoopWndProc)));
    ReleaseMouseInBackground(hwnd);
    Log("window: keeps running in the background during co-op, mouse free when another window is in front");
    for (const char *dll : {"PCs_Controls.dll", "GameUtils.dll", "XInterface.dll"})
        Log("window: %s %s", dll, GetModuleHandleA(dll) ? "hooked" : "not loaded");
}

// start.ini has no sections, so it can't be read with GetPrivateProfile*: "full_screen = 1".
bool StartIniFullScreen()
{
    FILE *f = fopen("start.ini", "r");
    if (!f)
        return false;
    char line[256];
    bool full = false;
    while (fgets(line, sizeof line, f))
    {
        int value = 0;
        if (sscanf(line, " full_screen = %d", &value) == 1)
            full = value != 0;
    }
    fclose(f);
    return full;
}

// Borderless needs the picture in the monitor's own size, or it is stretched: before the renderer reads
// start.ini (modules are loaded first), screen_x/screen_y are set to the primary monitor's size.
void FitStartIniToMonitor()
{
    char setting[8] = {};
    GetPrivateProfileStringA("coop", "borderless", "1", setting, sizeof setting, ".\\coop.ini");
    if (setting[0] == '0' || StartIniFullScreen())
        return;
    int width = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);
    FILE *f = fopen("start.ini", "rb");
    if (!f || width <= 0 || height <= 0)
    {
        if (f)
            fclose(f);
        return;
    }
    std::string text, line;
    bool changed = false;
    int c;
    while ((c = fgetc(f)) != EOF)
    {
        line += static_cast<char>(c);
        if (c != '\n')
            continue;
        int value = 0;
        if (sscanf(line.c_str(), " screen_x = %d", &value) == 1 && value != width)
        {
            line = "screen_x = " + std::to_string(width) + "\r\n";
            changed = true;
        }
        else if (sscanf(line.c_str(), " screen_y = %d", &value) == 1 && value != height)
        {
            line = "screen_y = " + std::to_string(height) + "\r\n";
            changed = true;
        }
        text += line;
        line.clear();
    }
    text += line;
    fclose(f);
    if (!changed)
        return;
    if (FILE *out = fopen("start.ini", "wb"))
    {
        fwrite(text.data(), 1, text.size(), out);
        fclose(out);
        Log("window: start.ini set to the monitor's %dx%d", width, height);
    }
}

// Exclusive full screen doesn't go with the above: Direct3D only lets go of the screen (Alt+Tab) when
// the window learns it was deactivated. So the game runs windowed (start.ini full_screen = 0) and the
// window is made borderless over its whole monitor: it looks like full screen, Alt+Tab works as for any
// window. coop.ini borderless=0 keeps the ordinary window.
void MakeBorderless()
{
    static bool done = false;
    if (done)
        return;
    HWND hwnd = nullptr;
    EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&hwnd));
    if (!hwnd)
        return;
    done = true;
    char setting[8] = {};
    GetPrivateProfileStringA("coop", "borderless", "1", setting, sizeof setting, ".\\coop.ini");
    if (setting[0] == '0' || StartIniFullScreen())
        return;
    MONITORINFO mi = {sizeof mi};
    if (!GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi))
        return;
    const RECT &r = mi.rcMonitor;
    SetWindowLongPtrA(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowLongPtrA(hwnd, GWL_EXSTYLE, 0);
    SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    Log("window: borderless over the monitor, %ldx%ld", r.right - r.left, r.bottom - r.top);
}

// ---- shared clock --------------------------------------------------------------

void SendClock(bool reliable = false)
{
    double gameSeconds = g_clock.Now(RealNow());
    double rate = g_clock.Rate();
    coopnet::Writer w;
    w.U16(kMsgClock);
    w.Raw(&gameSeconds, sizeof gameSeconds);
    w.Raw(&rate, sizeof rate);
    w.U32(g_clock.Epoch());
    Net().Send(coopnet::kBroadcast, w.Data(), reliable);
}

void SendPlayerTime()
{
    coopnet::Writer w;
    w.U16(kMsgPlayerTime);
    w.U32(static_cast<uint32_t>(g_local.mode));
    w.U8(g_local.battle ? 1 : 0);
    w.Raw(&g_local.rate, sizeof g_local.rate);
    Net().Send(coopnet::kHostId, w.Data(), false);
}

void HostOpenVote(int requester, uint32_t tag, double seconds);
void HostResolveVotes();
void OnSkipVote(uint32_t voteId, int requester, const std::string &name, double seconds);
void OnSkipResult(uint32_t voteId, int requester, uint32_t tag, bool approved, double seconds);

void HandleSkipMessage(int from, uint16_t type, coopnet::Reader &r)
{
    bool hosting = Net().GetState() == coopnet::State::Hosting;
    uint32_t id, tag, requester;
    uint8_t flag;
    double seconds;
    std::string name;
    switch (type)
    {
    case kMsgSkipRequest:
        if (hosting && r.U32(tag) && r.Raw(&seconds, sizeof seconds))
            HostOpenVote(from, tag, seconds);
        break;
    case kMsgSkipAnswer:
        if (hosting && r.U32(id) && r.U8(flag))
        {
            g_votes.Answer(id, from, flag != 0);
            HostResolveVotes();
        }
        break;
    case kMsgSkipVote:
        if (!hosting && r.U32(id) && r.U32(requester) && r.Str(name) && r.Raw(&seconds, sizeof seconds))
            OnSkipVote(id, static_cast<int>(requester), name, seconds);
        break;
    case kMsgSkipResult:
        if (!hosting && r.U32(id) && r.U32(requester) && r.U32(tag) && r.U8(flag) && r.Raw(&seconds, sizeof seconds))
            OnSkipResult(id, static_cast<int>(requester), tag, flag != 0, seconds);
        break;
    }
}

void HandleInternal(int from, uint16_t type, coopnet::Reader &r)
{
    if (type >= kMsgSkipRequest && type <= kMsgSkipResult)
    {
        HandleSkipMessage(from, type, r);
        return;
    }
    if (type == kMsgClock && Net().GetState() == coopnet::State::Connected)
    {
        double gameSeconds, rate;
        uint32_t epoch;
        if (r.Raw(&gameSeconds, sizeof gameSeconds) && r.Raw(&rate, sizeof rate) && r.U32(epoch))
        {
            if (!g_clock.Valid())
                Log("clock: following the host");
            g_clock.OnHostClock(gameSeconds, rate, epoch, RealNow());
        }
    }
    else if (type == kMsgPlayerTime && g_clock.IsHost())
    {
        uint32_t mode;
        uint8_t battle;
        coopnet::PlayerTime t;
        if (r.U32(mode) && r.U8(battle) && r.Raw(&t.rate, sizeof t.rate))
        {
            t.mode = static_cast<coopnet::Mode>(mode);
            t.battle = battle != 0;
            g_clock.SetPlayer(from, t);
        }
    }
}

// int CoopNet_Update()  once per script tick, before the CoopNet_Recv loop
DWORD __cdecl N_Update(VS_STACK *s)
{
    double now = RealNow();
    g_lastUpdate = now;
    g_local.rate = g_fixedRate > 0 ? g_fixedRate : g_meter.Rate();
    MakeBorderless();
    static long reported = 0;
    static double reportedAt = 0;
    if (g_mouseHeldBack != reported && now - reportedAt > 60.0) // once a minute at most
    {
        reported = g_mouseHeldBack;
        reportedAt = now;
        Log("window: %ld mouse moves held back while the game is behind", g_mouseHeldBack);
    }
    if (Net().GetState() != coopnet::State::Offline)
        KeepRunningInBackground();
    switch (Net().GetState())
    {
    case coopnet::State::Hosting:
        HostResolveVotes(); // timeouts
        if (g_clock.Valid())
        {
            g_clock.SetPlayer(coopnet::kHostId, g_local);
            g_clock.AdvanceHost(now);
            if (now - g_lastClockSend >= kClockBroadcastEvery)
            {
                g_lastClockSend = now;
                SendClock();
            }
        }
        break;
    case coopnet::State::Connected:
        if (now - g_lastPlayerTimeSend >= kPlayerTimeEvery)
        {
            g_lastPlayerTimeSend = now;
            SendPlayerTime();
        }
        break;
    default:
        if (g_clock.Valid())
        {
            g_clock.Reset();
            Log("clock: stopped");
        }
        break;
    }
    return Return(s, 1L);
}

// int CoopNet_TickAge()  ms since the last CoopNet_Update (detects a tick lost to ClearEvents)
DWORD __cdecl N_TickAge(VS_STACK *s)
{
    double age = g_lastUpdate < 0 ? 1e6 : (RealNow() - g_lastUpdate) * 1000.0;
    return Return(s, static_cast<long>(age > 1e6 ? 1e6 : age));
}

// float CoopNet_Seconds()  real seconds since the game started (for timing in scripts)
DWORD __cdecl N_Seconds(VS_STACK *s)
{
    static const double start = RealNow();
    return Return(s, static_cast<float>(RealNow() - start));
}

// int CoopNet_SetStatus(int mode, int battle)  mode: 0 menu, 1 world map, 2 sea, 3 land
DWORD __cdecl N_SetStatus(VS_STACK *s)
{
    bool battle = GetLong(Pop(s)) != 0;
    auto mode = static_cast<coopnet::Mode>(GetLong(Pop(s)));
    if (mode != g_local.mode)
    {
        g_meter.Reset(); // a new mode has its own pace
        g_fixedRate = 0;
    }
    if (mode != g_local.mode || battle != g_local.battle)
        Log("status: mode %d%s", static_cast<int>(mode), battle ? ", battle" : "");
    g_local.mode = mode;
    g_local.battle = battle;
    return Return(s, 1L);
}

// int CoopNet_SetLocalRate(float gameSecondsPerSecond)  pace of the current mode when it can't be
// measured (world map); call after CoopNet_SetStatus
DWORD __cdecl N_SetLocalRate(VS_STACK *s)
{
    g_fixedRate = GetFloat(Pop(s));
    return Return(s, 1L);
}

// float CoopNet_ClockEngine(int year, int month, int day, float hours, int source)
// The engine proposes a time; returns the hours to apply. The date to apply is then read with
// CoopNet_ClockYear/Month/Day, and CoopNet_ClockDaysAhead tells how many days it moved forward.
DWORD __cdecl N_ClockEngine(VS_STACK *s)
{
    long source = GetLong(Pop(s));
    float hours = GetFloat(Pop(s));
    long day = GetLong(Pop(s));
    long month = GetLong(Pop(s));
    long year = GetLong(Pop(s));
    double now = RealNow();
    coopnet::GameDate engine{static_cast<int>(year), static_cast<int>(month), static_cast<int>(day), hours};

    // 0: applying the clock on demand (after a skip). The world map keeps its own clock and ignores
    // the time we hand back, so its pace is reported by CoopNet_SetLocalRate instead of measured.
    bool measure = source != 0 && source != kClockSourceWorldMap;
    if (measure)
        g_meter.Sample(source, hours, now);
    auto state = Net().GetState();
    // The host's world defines the time: start from it once the host is actually in a game.
    if (state == coopnet::State::Hosting && !g_clock.Valid() && g_local.mode != coopnet::Mode::Menu)
    {
        g_clock.StartHost(engine, now, ++g_hostEpoch);
        Log("clock: hosting from %04d-%02d-%02d %.2f h, epoch %u", engine.year, engine.month, engine.day,
            engine.hours, g_hostEpoch);
    }

    g_applied = engine;
    if (g_clock.Valid() && (state == coopnet::State::Hosting || state == coopnet::State::Connected))
        g_applied = coopnet::FromSeconds(g_clock.Now(now));
    if (source != kClockSourceWorldMap)
        g_meter.Applied(g_applied.hours); // also after a skip: the engine steps on from the new time

    coopnet::GameDate a = g_applied, e = engine;
    a.hours = e.hours = 0;
    g_appliedDaysAhead = static_cast<int>((coopnet::ToSeconds(a) - coopnet::ToSeconds(e)) / 86400.0);
    return Return(s, static_cast<float>(g_applied.hours));
}

// int CoopNet_ClockRestart()  host: a new game or a loaded save defines the time from now on
DWORD __cdecl N_ClockRestart(VS_STACK *s)
{
    if (g_clock.IsHost())
    {
        g_clock.Reset();
        Log("clock: restart, waiting for the host's game time");
    }
    return Return(s, 1L);
}

DWORD __cdecl N_ClockYear(VS_STACK *s) { return Return(s, static_cast<long>(g_applied.year)); }
DWORD __cdecl N_ClockMonth(VS_STACK *s) { return Return(s, static_cast<long>(g_applied.month)); }
DWORD __cdecl N_ClockDay(VS_STACK *s) { return Return(s, static_cast<long>(g_applied.day)); }
DWORD __cdecl N_ClockDaysAhead(VS_STACK *s) { return Return(s, static_cast<long>(g_appliedDaysAhead)); }

// float CoopNet_ClockRate()  shared pace, game seconds per real second (0 = stopped)
DWORD __cdecl N_ClockRate(VS_STACK *s) { return Return(s, static_cast<float>(g_clock.Rate())); }

// ---- time-skip votes -----------------------------------------------------------
// Any skip (sleep, wait, repairs...) becomes a request to the host. The host asks everybody
// else; when all agree it moves the shared clock and everyone's time jumps together.

// Text for the game's UI, which uses Windows-1251.
std::string Cp1251(const std::wstring &text)
{
    int n = WideCharToMultiByte(1251, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(1251, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string &cp1251)
{
    int n = MultiByteToWideChar(1251, 0, cp1251.c_str(), static_cast<int>(cp1251.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(1251, 0, cp1251.c_str(), static_cast<int>(cp1251.size()), out.data(), n);
    return out;
}

std::wstring Duration(double seconds)
{
    long minutes = std::lround(seconds / 60.0);
    long days = minutes / 1440, hours = minutes % 1440 / 60, mins = minutes % 60;
    std::wstring s;
    if (days)
        s += std::to_wstring(days) + L" дн. ";
    if (hours)
        s += std::to_wstring(hours) + L" ч ";
    if (mins || s.empty())
        s += std::to_wstring(mins) + L" мин ";
    s.pop_back();
    return s;
}

void PushLocalEvent(uint16_t type, coopnet::Writer &fields)
{
    coopnet::Writer w;
    w.U16(type);
    w.Raw(fields.Data().data(), fields.Data().size());
    g_localEvents.push_back(coopnet::Message{coopnet::kHostId, std::move(w.Data())});
}

void Notice(const std::wstring &text)
{
    coopnet::Writer f;
    coopnet::WriteString(f, Cp1251(text));
    PushLocalEvent(kEvtNotice, f);
}

void OnSkipVote(uint32_t voteId, int requester, const std::string &name, double seconds)
{
    if (requester == Net().MyId())
    {
        Notice(L"Ждём согласия остальных на пропуск " + Duration(seconds));
        return;
    }
    g_voteToAnswer = voteId;
    Log("skip %u: asked to agree, %.0f s for player %d", voteId, seconds, requester);
    coopnet::Writer f;
    coopnet::WriteString(f, Cp1251(Wide(name) + L" хочет пропустить " + Duration(seconds) +
                                   L". F8 — согласиться, иначе через 30 с отказ"));
    PushLocalEvent(kEvtSkipVote, f);
}

void OnSkipResult(uint32_t voteId, int requester, uint32_t tag, bool approved, double seconds)
{
    if (g_voteToAnswer == voteId)
        g_voteToAnswer = 0;
    bool mine = requester == Net().MyId();
    std::string continuation;
    if (mine)
    {
        auto it = g_skipContinue.find(tag);
        if (it != g_skipContinue.end())
        {
            continuation = it->second;
            g_skipContinue.erase(it);
        }
    }
    std::wstring text = approved ? L"Время пропущено: " + Duration(seconds)
                        : voteId == 0 ? L"Пропуск времени: уже идёт другое голосование"
                                      : L"Пропуск времени отклонён";
    Log("skip %u: %s, %.0f s, requester %d", voteId, approved ? "approved" : "rejected", seconds, requester);
    coopnet::Writer f;
    coopnet::WriteInt(f, approved ? 1 : 0);
    coopnet::WriteInt(f, mine ? 1 : 0);
    coopnet::WriteString(f, continuation);
    coopnet::WriteString(f, Cp1251(text));
    PushLocalEvent(kEvtSkipResult, f);
}

void SendSkipResult(uint32_t voteId, int requester, uint32_t tag, bool approved, double seconds, int to)
{
    coopnet::Writer w;
    w.U16(kMsgSkipResult);
    w.U32(voteId);
    w.U32(static_cast<uint32_t>(requester));
    w.U32(tag);
    w.U8(approved ? 1 : 0);
    w.Raw(&seconds, sizeof seconds);
    Net().Send(to, w.Data(), true);
}

// Host: decided votes move the clock and are announced to everybody.
void HostResolveVotes()
{
    coopnet::VoteResult r;
    while (g_votes.Poll(RealNow(), r))
    {
        if (r.approved)
        {
            g_clock.Skip(r.seconds, RealNow());
            SendClock(true); // reliable and ahead of the result, so clients jump before they hear about it
        }
        SendSkipResult(r.id, r.requester, r.requesterTag, r.approved, r.seconds, coopnet::kBroadcast);
        OnSkipResult(r.id, r.requester, r.requesterTag, r.approved, r.seconds);
    }
}

// Host: a request from any player, including the host itself.
void HostOpenVote(int requester, uint32_t tag, double seconds)
{
    std::vector<int> players = Net().Peers();
    players.push_back(coopnet::kHostId);
    uint32_t id = g_votes.Open(requester, tag, seconds, players, RealNow());
    if (id == 0)
    {
        if (requester == coopnet::kHostId)
            OnSkipResult(0, requester, tag, false, seconds);
        else
            SendSkipResult(0, requester, tag, false, seconds, requester);
        return;
    }
    if (g_votes.Active() && g_votes.ActiveId() == id)
    {
        std::string name = Net().PeerName(requester);
        coopnet::Writer w;
        w.U16(kMsgSkipVote);
        w.U32(id);
        w.U32(static_cast<uint32_t>(requester));
        w.Str(name);
        w.Raw(&seconds, sizeof seconds);
        Net().Send(coopnet::kBroadcast, w.Data(), true);
        OnSkipVote(id, requester, name, seconds);
    }
    HostResolveVotes(); // decided at once when nobody needs asking
}

// int CoopNet_SkipVoting()  1 if time skips must go through a vote right now
DWORD __cdecl N_SkipVoting(VS_STACK *s)
{
    auto state = Net().GetState();
    bool voting = g_clock.Valid() && g_local.mode != coopnet::Mode::Menu &&
                  (state == coopnet::State::Hosting || state == coopnet::State::Connected);
    return Return(s, voting ? 1L : 0L);
}

// int CoopNet_RequestSkip(int minutes, string continuation)  continuation: quest to complete afterwards
DWORD __cdecl N_RequestSkip(VS_STACK *s)
{
    std::string continuation = ArgString(Pop(s));
    long minutes = GetLong(Pop(s));
    double seconds = minutes * 60.0;
    uint32_t tag = ++g_nextSkipTag;
    g_skipContinue[tag] = continuation;
    Log("skip request %u: %ld min%s%s", tag, minutes, continuation.empty() ? "" : ", then ", continuation.c_str());
    if (Net().GetState() == coopnet::State::Hosting)
        HostOpenVote(coopnet::kHostId, tag, seconds);
    else
    {
        coopnet::Writer w;
        w.U16(kMsgSkipRequest);
        w.U32(tag);
        w.Raw(&seconds, sizeof seconds);
        Net().Send(coopnet::kHostId, w.Data(), true);
    }
    return Return(s, 1L);
}

// int CoopNet_VoteYes()  agree to the time skip this player was asked about
DWORD __cdecl N_VoteYes(VS_STACK *s)
{
    uint32_t id = g_voteToAnswer;
    if (id == 0)
        return Return(s, 0L);
    g_voteToAnswer = 0;
    if (Net().GetState() == coopnet::State::Hosting)
    {
        g_votes.Answer(id, coopnet::kHostId, true);
        HostResolveVotes();
    }
    else
    {
        coopnet::Writer w;
        w.U16(kMsgSkipAnswer);
        w.U32(id);
        w.U8(1);
        Net().Send(coopnet::kHostId, w.Data(), true);
    }
    Notice(L"Вы согласились на пропуск времени");
    return Return(s, 1L);
}

// ---- outgoing messages -------------------------------------------------------

// int CoopNet_Begin(int type)  type >= 100
DWORD __cdecl N_Begin(VS_STACK *s)
{
    long type = GetLong(Pop(s));
    g_out.Clear();
    g_out.U16(static_cast<uint16_t>(type));
    return Return(s, 1L);
}

// int CoopNet_PutInt(int v)
DWORD __cdecl N_PutInt(VS_STACK *s)
{
    coopnet::WriteInt(g_out, GetLong(Pop(s)));
    return Return(s, 1L);
}

// int CoopNet_PutFloat(float v)
DWORD __cdecl N_PutFloat(VS_STACK *s)
{
    coopnet::WriteFloat(g_out, GetFloat(Pop(s)));
    return Return(s, 1L);
}

// int CoopNet_PutString(string v)
DWORD __cdecl N_PutString(VS_STACK *s)
{
    coopnet::WriteString(g_out, ArgString(Pop(s)));
    return Return(s, 1L);
}

// ---- world map ships driven by the script ---------------------------------------------------------
// WorldMap.dll can only create an encounter: a ship reads its position and heading once, when it is
// made (WdmEnemyShip::SetSaveAttribute), so a ship that has to follow another player's would be made
// again and again (a jump, a random heading, the old one fading out next to it). Instead the ship
// object is found in memory once and driven every frame. It is created with a kMaxSpeed no other ship
// has; WdmShip keeps ax, ay, az, dltAx, dltAz, speed, turnspd, baseV, kMaxSpeed in a row, and its
// matrix (position x, y, z, 1 in the last row) a little before them. Every access is guarded, and
// before each write the matrix and the zeroed speed are checked: a ship the map has let go is dropped,
// never written.
struct MapShip
{
    float key = 0;
    float *ax = nullptr;  // ay = ax[1], speed = ax[5], turnspd = ax[6], kMaxSpeed = ax[8]
    float *pos = nullptr; // x, y, z, 1
    float x = 0, z = 0, vx = 0, vz = 0, ay = 0;
    float lastX = 0, lastZ = 0, lastAy = 0; // where it is shown, which glides to where it should be
    double at = 0;
    double drivenAt = 0;
    bool hasAy = false;
    float maxStep = 0, maxOff = 0; // for the log: the biggest correction in a frame, the biggest gap
    std::string label; // a name drawn under it ("": none)
    double loggedAt = 0;
};
std::map<std::string, MapShip> g_mapShips;

// A player's name on the map with no ship under it, kept 3 s after its last word.
struct MapMark
{
    float x = 0, z = 0;
    std::string text;
    double at = 0;
};
std::map<long, MapMark> g_mapMarks;

// Whether the floats before a kMaxSpeed at k look like a WdmShip standing at x, z.
bool ProbeMapShip(float *k, float x, float z, float **outAx, float **outPos)
{
    __try
    {
        float *ax = k - 8;
        for (int i = 0; i < 8; ++i)
            if (!std::isfinite(ax[i]))
                return false;
        if (std::fabs(ax[1]) > 100.0f || std::fabs(ax[5]) > 1000.0f)
            return false;
        for (int back = 0; back < 400; ++back)
        {
            float *p = ax - 4 - back;
            if (p[3] == 1.0f && std::fabs(p[1]) < 5.0f && std::fabs(p[0] - x) < 3.0f && std::fabs(p[2] - z) < 3.0f)
            {
                *outAx = ax;
                *outPos = p;
                return true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return false;
}

// Looks through one memory region for the bits of key; up to max finds go to found.
int ScanRegion(const uint32_t *from, size_t count, uint32_t key, float **found, int have, int max)
{
    __try
    {
        for (size_t i = 0; i < count && have < max; ++i)
            if (from[i] == key)
                found[have++] = const_cast<float *>(reinterpret_cast<const float *>(from + i));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return have;
}

bool DriveMapShip(float *ax, float *pos, float x, float z, float ay, float lastX, float lastZ, float key)
{
    __try
    {
        if (pos[3] != 1.0f || std::fabs(pos[1]) > 5.0f)
            return false;
        if (ax[8] != 0.0f && ax[8] != key)
            return false;
        if (std::fabs(pos[0] - lastX) > 5.0f || std::fabs(pos[2] - lastZ) > 5.0f)
            return false; // not where it was left: no longer that ship
        pos[0] = x;
        pos[2] = z;
        ax[1] = ay;
        ax[5] = 0.0f; // its own speed and turning stay off: it only goes where it is put
        ax[6] = 0.0f;
        ax[8] = 0.0f;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return false;
}

// int CoopNet_MapShipBind(string enc, float kSpeed, float x, float z)  just after creating it with
// that kSpeed at x, z: 1 when the ship was found and will be driven from now on
DWORD __cdecl N_MapShipBind(VS_STACK *s)
{
    float z = GetFloat(Pop(s));
    float x = GetFloat(Pop(s));
    float key = GetFloat(Pop(s));
    std::string enc = ArgString(Pop(s));
    if (key < 0.1f)
        key = 0.1f; // CreateMerchantShipXZ does the same
    uint32_t bits;
    memcpy(&bits, &key, sizeof bits);
    double started = RealNow();
    MapShip ship;
    int ships = 0;
    int have = 0;
    // the regions where ships were found before first (ships come from the same heap), then all
    static std::vector<BYTE *> hot;
    bool fromHot = false;
    for (int pass = 0; pass < 2 && ships == 0; ++pass)
    {
        float *found[64];
        have = 0;
        MEMORY_BASIC_INFORMATION mbi;
        if (pass == 0)
        {
            for (BYTE *base : hot)
                if (VirtualQuery(base, &mbi, sizeof mbi) == sizeof mbi && mbi.State == MEM_COMMIT &&
                    mbi.Protect == PAGE_READWRITE)
                    have = ScanRegion(static_cast<const uint32_t *>(mbi.BaseAddress), mbi.RegionSize / 4, bits, found,
                                      have, 64);
        }
        else
        {
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            auto addr = static_cast<BYTE *>(si.lpMinimumApplicationAddress);
            while (addr < si.lpMaximumApplicationAddress && have < 64 &&
                   VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi)
            {
                if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE)
                    have = ScanRegion(static_cast<const uint32_t *>(mbi.BaseAddress), mbi.RegionSize / 4, bits, found,
                                      have, 64);
                addr = static_cast<BYTE *>(mbi.BaseAddress) + mbi.RegionSize;
            }
        }
        for (int i = 0; i < have; ++i)
        {
            float *ax = nullptr, *pos = nullptr;
            if (ProbeMapShip(found[i], x, z, &ax, &pos))
            {
                ++ships;
                ship.ax = ax;
                ship.pos = pos;
            }
        }
        fromHot = pass == 0;
    }
    if (ships == 1 && !fromHot)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(ship.ax, &mbi, sizeof mbi) == sizeof mbi)
            hot.push_back(static_cast<BYTE *>(mbi.BaseAddress));
    }
    Log("map ship %s: kMaxSpeed %g found %d times, %d look like a ship at %.2f %.2f (%.0f ms%s)", enc.c_str(), key, have,
        ships, x, z, (RealNow() - started) * 1000.0, fromHot ? ", known region" : "");
    if (ships != 1)
        return Return(s, 0L);
    ship.key = key;
    coopnet::HookMapLabels();
    ship.x = ship.lastX = x;
    ship.z = ship.lastZ = z;
    ship.at = ship.drivenAt = RealNow();
    g_mapShips[enc] = ship;
    return Return(s, 1L);
}

// int CoopNet_MapShipSet(string enc, float x, float z, float vx, float vz, float ay)  where a driven
// ship is now and how it moves (map units per second), till the next word
DWORD __cdecl N_MapShipSet(VS_STACK *s)
{
    float ay = GetFloat(Pop(s));
    float vz = GetFloat(Pop(s));
    float vx = GetFloat(Pop(s));
    float z = GetFloat(Pop(s));
    float x = GetFloat(Pop(s));
    std::string enc = ArgString(Pop(s));
    auto it = g_mapShips.find(enc);
    if (it == g_mapShips.end())
        return Return(s, 0L);
    MapShip &m = it->second;
    m.x = x;
    m.z = z;
    m.vx = vx;
    m.vz = vz;
    m.ay = ay;
    m.at = RealNow();
    if (!m.hasAy)
    {
        m.hasAy = true;
        m.lastAy = ay;
    }
    return Return(s, 1L);
}

// The shortest turn from a to b, in radians.
float TurnBetween(float a, float b)
{
    float d = std::fmod(b - a, 6.2831853f);
    if (d > 3.1415927f)
        d -= 6.2831853f;
    if (d < -3.1415927f)
        d += 6.2831853f;
    return d;
}

// int CoopNet_MapShipDrive(float meX, float meZ, int show)  every frame on the world map: puts every driven ship
// where it is now; meX, meZ: this player's own ship (for the names drawn over the others); show 0: no
// names (a window of the game is open over the map)
DWORD __cdecl N_MapShipDrive(VS_STACK *s)
{
    long show = GetLong(Pop(s));
    float meZ = GetFloat(Pop(s));
    float meX = GetFloat(Pop(s));
    // Reports come twice a second, each with its own small error: snapping to every one makes the ship
    // jerk. So the ship shown sails on with the reported speed and closes the gap to where the player
    // should be over about half a second (it jumps only when it is far off), and turns gradually.
    std::vector<coopnet::MapLabel> labels;
    double now = RealNow();
    for (auto it = g_mapShips.begin(); it != g_mapShips.end();)
    {
        MapShip &m = it->second;
        float dt = static_cast<float>(now - m.drivenAt);
        if (dt > 0.5f)
            dt = 0.5f;
        m.drivenAt = now;
        double age = now - m.at;
        if (age > 3.0)
            age = 3.0;
        float tx = m.x + m.vx * static_cast<float>(age);
        float tz = m.z + m.vz * static_cast<float>(age);
        float x = m.lastX + m.vx * dt;
        float z = m.lastZ + m.vz * dt;
        float ex = tx - x, ez = tz - z;
        if (ex * ex + ez * ez > 30.0f * 30.0f)
        {
            x = tx;
            z = tz;
        }
        else
        {
            float k = dt * 2.0f;
            if (k > 1.0f)
                k = 1.0f;
            x += ex * k;
            z += ez * k;
            float step = std::sqrt(ex * ex + ez * ez) * k;
            if (step > m.maxStep)
                m.maxStep = step;
        }
        float turn = TurnBetween(m.lastAy, m.ay);
        float maxTurn = 2.0f * dt; // radians per second
        if (turn > maxTurn)
            turn = maxTurn;
        if (turn < -maxTurn)
            turn = -maxTurn;
        float ay = m.lastAy + turn;
        if (!DriveMapShip(m.ax, m.pos, x, z, ay, m.lastX, m.lastZ, m.key))
        {
            Log("map ship %s let go", it->first.c_str());
            it = g_mapShips.erase(it);
            continue;
        }
        m.lastX = x;
        m.lastZ = z;
        m.lastAy = ay;
        if (!m.label.empty() && show)
            labels.push_back({x, 0.0f, z, m.label});
        float off = std::sqrt((tx - x) * (tx - x) + (tz - z) * (tz - z));
        if (off > m.maxOff)
            m.maxOff = off;
        if (now - m.loggedAt > 10.0)
        {
            Log("map ship %s: biggest correction in a frame %.3f, biggest gap %.2f (10 s)", it->first.c_str(), m.maxStep,
                m.maxOff);
            m.loggedAt = now;
            m.maxStep = m.maxOff = 0;
        }
        ++it;
    }
    // players without a ship of their own here (not made yet, or not driven): their name where they are
    for (auto it = g_mapMarks.begin(); it != g_mapMarks.end();)
    {
        if (now - it->second.at > 3.0)
        {
            it = g_mapMarks.erase(it);
            continue;
        }
        if (show)
            labels.push_back({it->second.x, 0.0f, it->second.z, it->second.text});
        ++it;
    }
    coopnet::SetLabels(labels, meX, 0.0f, meZ, false);
    return Return(s, static_cast<long>(g_mapShips.size()));
}

// int CoopNet_MapShipLabel(string enc, string text)  a name drawn under a driven ship on the map
DWORD __cdecl N_MapShipLabel(VS_STACK *s)
{
    std::string text = ArgString(Pop(s));
    std::string enc = ArgString(Pop(s));
    auto it = g_mapShips.find(enc);
    if (it == g_mapShips.end())
        return Return(s, 0L);
    it->second.label = text;
    return Return(s, 1L);
}

// int CoopNet_MapShipForget(string enc)  stops driving that ship ("": all of them, the map is gone)
DWORD __cdecl N_MapShipForget(VS_STACK *s)
{
    std::string enc = ArgString(Pop(s));
    if (enc.empty())
        g_mapShips.clear();
    else
        g_mapShips.erase(enc);
    return Return(s, 1L);
}

// int CoopNet_MapMark(int id, float x, float z, string text)  player id's name drawn at x, z on the map
// while it has no driven ship there ("": none)
DWORD __cdecl N_MapMark(VS_STACK *s)
{
    std::string text = ArgString(Pop(s));
    float z = GetFloat(Pop(s));
    float x = GetFloat(Pop(s));
    long id = GetLong(Pop(s));
    if (text.empty())
    {
        g_mapMarks.erase(id);
        return Return(s, 1L);
    }
    MapMark &m = g_mapMarks[id];
    m.x = x;
    m.z = z;
    m.text = text;
    m.at = RealNow();
    return Return(s, 1L);
}

// At sea: other players' ships with their names over them, given every frame.
struct SeaMark
{
    float x = 0, y = 0, z = 0;
    std::string text;
    double at = 0;
};
std::map<long, SeaMark> g_seaMarks;

// int CoopNet_SeaMark(int id, float x, float y, float z, string text)  player id's name at x, y, z at sea
DWORD __cdecl N_SeaMark(VS_STACK *s)
{
    std::string text = ArgString(Pop(s));
    float z = GetFloat(Pop(s));
    float y = GetFloat(Pop(s));
    float x = GetFloat(Pop(s));
    long id = GetLong(Pop(s));
    SeaMark &m = g_seaMarks[id];
    m.x = x;
    m.y = y;
    m.z = z;
    m.text = text;
    m.at = RealNow();
    return Return(s, 1L);
}

// int CoopNet_SeaDrive(float meX, float meY, float meZ)  every frame at sea: the names given in the last
// second are drawn; meX, meY, meZ: this player's own ship (by which the camera is found)
DWORD __cdecl N_SeaDrive(VS_STACK *s)
{
    float meZ = GetFloat(Pop(s));
    float meY = GetFloat(Pop(s));
    float meX = GetFloat(Pop(s));
    coopnet::HookMapLabels();
    std::vector<coopnet::MapLabel> labels;
    double now = RealNow();
    for (auto it = g_seaMarks.begin(); it != g_seaMarks.end();)
    {
        if (now - it->second.at > 1.0)
        {
            it = g_seaMarks.erase(it);
            continue;
        }
        labels.push_back({it->second.x, it->second.y, it->second.z, it->second.text});
        ++it;
    }
    coopnet::SetLabels(labels, meX, meY, meZ, true);
    return Return(s, static_cast<long>(labels.size()));
}

// int CoopNet_SetPanel(string text)  the player list F3 shows, lines split by '|'
DWORD __cdecl N_SetPanel(VS_STACK *s)
{
    std::string text = ArgString(Pop(s));
    coopnet::HookMapLabels();
    coopnet::SetPanel(text);
    return Return(s, 1L);
}

// int CoopNet_TestKey(int vk, int down)  test runs only: holds a key down or lets it go
DWORD __cdecl N_TestKey(VS_STACK *s)
{
    long down = GetLong(Pop(s));
    long vk = GetLong(Pop(s));
    if (vk < 0 || vk >= 256)
        return Return(s, 0L);
    g_testKeys[vk] = down != 0;
    return Return(s, 1L);
}

// int CoopNet_Send(int to, int reliable)  to: peer id, 0 = host, -1 = everyone
DWORD __cdecl N_Send(VS_STACK *s)
{
    long reliable = GetLong(Pop(s));
    long to = GetLong(Pop(s));
    if (g_out.Data().size() < 2)
    {
        Log("CoopNet_Send without CoopNet_Begin");
        return Return(s, 0L);
    }
    return Return(s, Net().Send(to, g_out.Data(), reliable != 0) ? 1L : 0L);
}

// ---- incoming messages -------------------------------------------------------

// int CoopNet_Recv()  type of the next message, 0 if none
DWORD __cdecl N_Recv(VS_STACK *s)
{
    if (!g_localEvents.empty())
    {
        g_in = std::move(g_localEvents.front());
        g_localEvents.pop_front();
        g_inRd = coopnet::Reader(g_in.payload);
        uint16_t type = 0;
        g_inRd.U16(type);
        return Return(s, static_cast<long>(type));
    }
    while (Net().Poll(g_in))
    {
        g_inRd = coopnet::Reader(g_in.payload);
        uint16_t type = 0;
        if (!g_inRd.U16(type))
            continue;
        if (type >= kFirstInternal && type <= kLastInternal)
        {
            HandleInternal(g_in.from, type, g_inRd);
            continue;
        }
        if (type == coopnet::kEvtPeerLeft)
        {
            g_clock.RemovePlayer(g_in.from);
            g_votes.PlayerLeft(g_in.from);
            HostResolveVotes();
        }
        return Return(s, static_cast<long>(type));
    }
    g_in = coopnet::Message{};
    g_inRd = coopnet::Reader();
    return Return(s, 0L);
}

// int CoopNet_From()  sender of the last received message
DWORD __cdecl N_From(VS_STACK *s) { return Return(s, static_cast<long>(g_in.from)); }

// int CoopNet_GetInt()
DWORD __cdecl N_GetInt(VS_STACK *s)
{
    int32_t v = 0;
    if (!coopnet::ReadInt(g_inRd, v))
        Log("CoopNet_GetInt: next field is not an int");
    return Return(s, static_cast<long>(v));
}

// float CoopNet_GetFloat()
DWORD __cdecl N_GetFloat(VS_STACK *s)
{
    float v = 0;
    if (!coopnet::ReadFloat(g_inRd, v))
        Log("CoopNet_GetFloat: next field is not a float");
    return Return(s, v);
}

// string CoopNet_GetString()
DWORD __cdecl N_GetString(VS_STACK *s)
{
    std::string v;
    if (!coopnet::ReadString(g_inRd, v))
        Log("CoopNet_GetString: next field is not a string");
    return Return(s, v);
}

// ---- misc --------------------------------------------------------------------

// int CoopNet_Version()
DWORD __cdecl N_Version(VS_STACK *s) { return Return(s, kVersion); }

// int CoopNet_Log(string s)
DWORD __cdecl N_Log(VS_STACK *s)
{
    Log("script: %s", ArgString(Pop(s)).c_str());
    return Return(s, 1L);
}

// string CoopNet_GetEnv(string name)  "" if not set; used to start test instances
DWORD __cdecl N_GetEnv(VS_STACK *s)
{
    std::string name = ArgString(Pop(s));
    char buf[256];
    DWORD n = GetEnvironmentVariableA(name.c_str(), buf, sizeof buf);
    return Return(s, n > 0 && n < sizeof buf ? std::string(buf, n) : std::string());
}

struct Native
{
    const char *name;
    const char *ret;
    DWORD args;
    SIMPORTFUNC fn;
};

const Native kNatives[] = {
    {"CoopNet_Host", "int", 3, N_Host},
    {"CoopNet_Connect", "int", 3, N_Connect},
    {"CoopNet_Disconnect", "int", 0, N_Disconnect},
    {"CoopNet_State", "int", 0, N_State},
    {"CoopNet_MyId", "int", 0, N_MyId},
    {"CoopNet_PeerCount", "int", 0, N_PeerCount},
    {"CoopNet_PeerId", "int", 1, N_PeerId},
    {"CoopNet_PeerName", "string", 1, N_PeerName},
    {"CoopNet_Ping", "int", 1, N_Ping},
    {"CoopNet_LastError", "string", 0, N_LastError},
    {"CoopNet_SetLoss", "int", 1, N_SetLoss},
    {"CoopNet_Begin", "int", 1, N_Begin},
    {"CoopNet_PutInt", "int", 1, N_PutInt},
    {"CoopNet_PutFloat", "int", 1, N_PutFloat},
    {"CoopNet_PutString", "int", 1, N_PutString},
    {"CoopNet_Send", "int", 2, N_Send},
    {"CoopNet_Recv", "int", 0, N_Recv},
    {"CoopNet_From", "int", 0, N_From},
    {"CoopNet_GetInt", "int", 0, N_GetInt},
    {"CoopNet_GetFloat", "float", 0, N_GetFloat},
    {"CoopNet_GetString", "string", 0, N_GetString},
    {"CoopNet_Version", "int", 0, N_Version},
    {"CoopNet_Log", "int", 1, N_Log},
    {"CoopNet_GetEnv", "string", 1, N_GetEnv},
    {"CoopNet_Update", "int", 0, N_Update},
    {"CoopNet_TickAge", "int", 0, N_TickAge},
    {"CoopNet_SetStatus", "int", 2, N_SetStatus},
    {"CoopNet_ClockEngine", "float", 5, N_ClockEngine},
    {"CoopNet_ClockYear", "int", 0, N_ClockYear},
    {"CoopNet_ClockMonth", "int", 0, N_ClockMonth},
    {"CoopNet_ClockDay", "int", 0, N_ClockDay},
    {"CoopNet_ClockDaysAhead", "int", 0, N_ClockDaysAhead},
    {"CoopNet_ClockRate", "float", 0, N_ClockRate},
    {"CoopNet_ClockRestart", "int", 0, N_ClockRestart},
    {"CoopNet_SkipVoting", "int", 0, N_SkipVoting},
    {"CoopNet_RequestSkip", "int", 2, N_RequestSkip},
    {"CoopNet_VoteYes", "int", 0, N_VoteYes},
    {"CoopNet_UseSteam", "int", 0, N_UseSteam},
    {"CoopNet_UseUdp", "int", 0, N_UseUdp},
    {"CoopNet_Config", "string", 1, N_Config},
    {"CoopNet_SetLocalRate", "int", 1, N_SetLocalRate},
    {"CoopNet_Seconds", "float", 0, N_Seconds},
    {"CoopNet_Invite", "int", 0, N_Invite},
    {"CoopNet_MyUid", "string", 0, N_MyUid},
    {"CoopNet_TestKey", "int", 2, N_TestKey},
    {"CoopNet_MapShipBind", "int", 4, N_MapShipBind},
    {"CoopNet_MapShipSet", "int", 6, N_MapShipSet},
    {"CoopNet_MapShipDrive", "int", 3, N_MapShipDrive},
    {"CoopNet_MapShipForget", "int", 1, N_MapShipForget},
    {"CoopNet_MapShipLabel", "int", 2, N_MapShipLabel},
    {"CoopNet_MapMark", "int", 4, N_MapMark},
    {"CoopNet_SeaMark", "int", 5, N_SeaMark},
    {"CoopNet_SeaDrive", "int", 3, N_SeaDrive},
    {"CoopNet_SetPanel", "int", 1, N_SetPanel},
    {"CoopNet_RollbackSave", "string", 1, N_RollbackSave},
    {"CoopNet_SaveLoaded", "int", 1, N_SaveLoaded},
};

class COOPNET : public SCRIPT_LIBRIARY
{
  public:
    bool Init() override
    {
        for (const Native &n : kNatives)
        {
            IFUNCINFO info;
            info.pFuncName = n.name;
            info.pReturnValueName = n.ret;
            info.nArguments = n.args;
            info.pFuncAddress = n.fn;
            SetScriptFunction(g_api, &info);
        }
        Log("COOPNET::Init v%ld, %zu natives", kVersion, sizeof kNatives / sizeof kNatives[0]);
        return true;
    }
};

class COOPNETvmacd : public VMA
{
  public:
    explicit COOPNETvmacd(VMA *&root)
    {
        pNext = root;
        root = this;
    }
    const char *GetName() override { return "COOPNET"; }
    void *CreateClass() override { return new COOPNET; }
    bool ScriptLibriary() override { return true; }
};

VMA *g_moduleClassRoot = nullptr;
COOPNETvmacd g_coopnetClass(g_moduleClassRoot);

} // namespace

void coopnet::LogLine(const char *line)
{
    Log("%s", line);
}

#ifndef COOPNET_STEAM
// Built without the Steamworks SDK (CoopMod/steamworks_sdk is not in git): UDP only.
std::unique_ptr<coopnet::Session> coopnet::CreateSteamSession(std::string &err)
{
    err = "built without the Steamworks SDK";
    return nullptr;
}
bool coopnet::StartSteamEarly(std::string &err)
{
    err = "built without the Steamworks SDK";
    return false;
}
#endif

extern "C" __declspec(dllexport) VMA *__cdecl DMAInterface(VAPI *api, VSYSTEM_API *)
{
    g_api = api;
    coopnet::SetSessionLog([](const char *line) { Log("%s", line); });
    GetSystemTimeAsFileTime(&g_launchTime);
    FitStartIniToMonitor();
    // Over Steam the overlay has to be in place before the renderer creates its device: modules are
    // loaded before that, scripts start after it.
    char transport[32] = {};
    GetPrivateProfileStringA("coop", "transport", "", transport, sizeof transport, ".\\coop.ini");
    if (_stricmp(transport, "steam") == 0)
    {
        std::string err;
        if (!coopnet::StartSteamEarly(err))
            Log("steam: not started at launch: %s", err.c_str());
    }
    return g_moduleClassRoot;
}
