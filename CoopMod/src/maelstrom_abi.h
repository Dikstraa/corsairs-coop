// Minimal ABI of Maelstrom Engine x64 (Maelstrom.exe, Dec 2021) for native modules.
//
// Based on Storm 2.x / sd-teho headers (storm-engine commit 76e8eaf), but the x64
// Maelstrom build differs from them, so engine interfaces are called through
// explicit vtable slots verified against the shipped modules/*.dll instead of
// mirroring the original class declarations.
#pragma once

#include <windows.h>

namespace mael
{

// Call virtual method #slot of an engine object (x64: `this` goes first, like a plain arg).
template <typename R, typename... A> inline R VCall(const void *obj, int slot, A... args)
{
    using Fn = R (*)(const void *, A...);
    return (*reinterpret_cast<Fn *const *>(obj))[slot](obj, args...);
}

// ---- VAPI (core) -----------------------------------------------------------
// SCRIPT_LIBRARY.dll: 14 calls through [api+0x228] == its 14 registered natives.
constexpr int VAPI_SetScriptFunction = 0x228 / 8;

// ---- VS_STACK (script call stack) -------------------------------------------
// SCRIPT_LIBRARY.dll _Clampf: Pop = [vt+10h], Push(0) = [vt+8].
constexpr int VSTACK_Push = 1;
constexpr int VSTACK_Pop = 2;

// ---- VDATA (script value) ---------------------------------------------------
// SCRIPT_LIBRARY.dll call counts: 0x190 x2 (long args), 0x198 x12 (float args), 0x1A0 x10 (string args).
constexpr int VDATA_GetLong = 0x190 / 8;
constexpr int VDATA_GetFloat = 0x198 / 8;
constexpr int VDATA_GetString = 0x1A0 / 8;
// MSVC lays out the 10 Set() overloads (slots 4..13) in reverse declaration order.
// Verified: _Clampf returns via Set(float) = [vt+60h]; bool natives via Set(long) = [vt+68h].
constexpr int VDATA_SetString = 0x58 / 8;
constexpr int VDATA_SetFloat = 0x60 / 8;
constexpr int VDATA_SetLong = 0x68 / 8;

#define IFUNCRESULT_OK 0
#define IFUNCRESULT_FAILED 0xffffffff

struct VS_STACK;
struct VDATA;
struct VAPI;
struct VSYSTEM_API;

typedef DWORD(__cdecl *SIMPORTFUNC)(VS_STACK *);

struct IFUNCINFO
{
    const char *pFuncName = nullptr;
    const char *pReturnValueName = nullptr;
    DWORD nArguments = 0;
    SIMPORTFUNC pFuncAddress = nullptr;
    const char *pDeclFileName = nullptr;
    DWORD nDeclLine = 0;
};

// Thin typed wrappers over the slots above.
inline VDATA *Push(VS_STACK *s) { return VCall<VDATA *>(s, VSTACK_Push, (VDATA *)nullptr); }
inline VDATA *Pop(VS_STACK *s) { return VCall<VDATA *>(s, VSTACK_Pop); }
inline long GetLong(VDATA *d) { return VCall<long>(d, VDATA_GetLong); }
inline float GetFloat(VDATA *d) { return VCall<float>(d, VDATA_GetFloat); }
inline const char *GetString(VDATA *d) { return VCall<const char *>(d, VDATA_GetString); }
inline void Set(VDATA *d, long v) { VCall<void>(d, VDATA_SetLong, v); }
inline void Set(VDATA *d, float v) { VCall<void>(d, VDATA_SetFloat, v); }
inline void Set(VDATA *d, const char *v) { VCall<void>(d, VDATA_SetString, v); }
inline DWORD SetScriptFunction(VAPI *api, IFUNCINFO *info) { return VCall<DWORD>(api, VAPI_SetScriptFunction, info); }

// ---- Module registration (layout verified via RTTI: VMA has 8 virtuals) ------
class VMA
{
  protected:
    VMA *pNext = nullptr;
    long nHash = 0;
    long nReference = 0;

  public:
    virtual ~VMA() {}
    virtual bool Service() { return false; }
    virtual const char *GetName() { return nullptr; }
    virtual void *CreateClass() { return nullptr; }
    virtual void RefDec() { nReference--; }
    virtual long GetReference() { return nReference; }
    virtual void Clear() { nReference = 0; }
    virtual bool ScriptLibriary() { return false; }
};

class SCRIPT_LIBRIARY
{
  public:
    virtual ~SCRIPT_LIBRIARY() {}
    virtual bool Init() { return false; }
};

} // namespace mael
