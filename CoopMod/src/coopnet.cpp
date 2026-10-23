// CoopNet: native script library (#library "COOPNET") exposing the co-op network session.
//
// Scripts build an outgoing message with CoopNet_Begin/Put*/Send and read incoming ones with
// CoopNet_Recv/From/Get*. Field encoding and system message types: see wire.h and session.h.
#include "maelstrom_abi.h"
#include "session.h"
#include "wire.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

using namespace mael;

namespace
{

constexpr long kVersion = 2;

VAPI *g_api = nullptr;
std::string g_lastError;

coopnet::Writer g_out;   // message being built by CoopNet_Begin/Put*
coopnet::Message g_in;   // message returned by the last CoopNet_Recv
coopnet::Reader g_inRd;  // read position inside g_in
std::string g_retString; // keeps returned strings alive after the native returns

// Created on first use and intentionally never destroyed: joining the network thread from a
// static destructor would run under the loader lock.
coopnet::Session &Net()
{
    static coopnet::Session *session = coopnet::CreateUdpSession().release();
    return *session;
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
    uint16_t type = 0;
    if (Net().Poll(g_in))
    {
        g_inRd = coopnet::Reader(g_in.payload);
        g_inRd.U16(type);
    }
    else
    {
        g_in = coopnet::Message{};
        g_inRd = coopnet::Reader();
    }
    return Return(s, static_cast<long>(type));
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

extern "C" __declspec(dllexport) VMA *__cdecl DMAInterface(VAPI *api, VSYSTEM_API *)
{
    g_api = api;
    return g_moduleClassRoot;
}
