// CoopNet: native script library for the co-op mod.
// Stage 1 (proof of concept): registers a few trivial natives to verify the engine ABI.
#include "maelstrom_abi.h"

#include <cstdio>
#include <string>

using namespace mael;

static VAPI *g_api = nullptr;
static std::string g_echo;

static void Log(const char *msg)
{
    if (FILE *f = fopen("coopnet.log", "a"))
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, msg);
        fclose(f);
    }
}

// int CoopNet_Version()
static DWORD __cdecl N_Version(VS_STACK *s)
{
    VDATA *r = Push(s);
    if (!r)
        return IFUNCRESULT_FAILED;
    Set(r, 1L);
    return IFUNCRESULT_OK;
}

// float CoopNet_AddF(float a, float b)
static DWORD __cdecl N_AddF(VS_STACK *s)
{
    VDATA *b = Pop(s);
    VDATA *a = Pop(s);
    VDATA *r = Push(s);
    if (!r)
        return IFUNCRESULT_FAILED;
    Set(r, GetFloat(a) + GetFloat(b));
    return IFUNCRESULT_OK;
}

// string CoopNet_Echo(string s)
static DWORD __cdecl N_Echo(VS_STACK *s)
{
    VDATA *a = Pop(s);
    const char *str = GetString(a);
    g_echo = std::string("echo:") + (str ? str : "<null>");
    VDATA *r = Push(s);
    if (!r)
        return IFUNCRESULT_FAILED;
    Set(r, g_echo.c_str());
    return IFUNCRESULT_OK;
}

// int CoopNet_Log(string s)
static DWORD __cdecl N_Log(VS_STACK *s)
{
    VDATA *a = Pop(s);
    const char *str = GetString(a);
    Log(str ? str : "<null>");
    VDATA *r = Push(s);
    if (!r)
        return IFUNCRESULT_FAILED;
    Set(r, 1L);
    return IFUNCRESULT_OK;
}

static void Register(const char *name, const char *ret, DWORD args, SIMPORTFUNC fn)
{
    IFUNCINFO info;
    info.pFuncName = name;
    info.pReturnValueName = ret;
    info.nArguments = args;
    info.pFuncAddress = fn;
    SetScriptFunction(g_api, &info);
}

class COOPNET : public SCRIPT_LIBRIARY
{
  public:
    bool Init() override
    {
        Log("COOPNET::Init");
        Register("CoopNet_Version", "int", 0, N_Version);
        Register("CoopNet_AddF", "float", 2, N_AddF);
        Register("CoopNet_Echo", "string", 1, N_Echo);
        Register("CoopNet_Log", "int", 1, N_Log);
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

static VMA *g_moduleClassRoot = nullptr;
static COOPNETvmacd g_coopnetClass(g_moduleClassRoot);

extern "C" __declspec(dllexport) VMA *__cdecl DMAInterface(VAPI *api, VSYSTEM_API *)
{
    g_api = api;
    Log("DMAInterface called");
    return g_moduleClassRoot;
}
