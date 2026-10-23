# Corsairs Ship Pack — Co-op mod

Co-op multiplayer mod for **MS Corsairs Ship Pack** (Maelstrom Engine x64).

The repository holds only the mod and the game scripts. Game binaries and `RESOURCE/`
come from an installed copy of the Ship Pack: clone this repo over the game folder.

## Layout

- `CoopMod/src/` — native module `CoopNet.dll` (script library `COOPNET`).
  - `maelstrom_abi.h` — engine interfaces with vtable slots verified against the stock `modules/*.dll`.
  - `session.h` — transport-independent session (host + clients, reliable/unreliable messages, events).
  - `udp_session.cpp` — UDP implementation: handshake, ordered reliable channel with acks/resends and
    fragmentation, pings, timeouts. Runs on its own thread, so connections survive while the game is paused.
  - `wire.h` — message encoding: `u16 type` + tagged int/float/string fields.
  - `coopnet.cpp` — script natives (`CoopNet_*`).
- `CoopMod/tests/session_test.cpp` — transport test: host + 3 clients over loopback with 30% packet loss.
- `CoopMod/build.bat` — builds the DLL (VS 2022 Build Tools, x64) and copies it to `modules/`.
- `CoopMod/test.bat` — builds and runs the transport test.
- `CoopMod/run_pair.ps1` — starts a host and a client game instance on one PC and prints `coopnet.log`.
- `Program/` — game scripts. The first commit is the unmodified Ship Pack; later commits are mod changes.
- `Program/coop/coopnet.c` — script side: constants, native declarations, message tick and dispatch.

## Script API

```c
CoopNet_Host(port, maxPeers, name);  CoopNet_Connect(host, port, name);  CoopNet_Disconnect();
CoopNet_State();  CoopNet_MyId();  CoopNet_PeerCount();  CoopNet_PeerId(i);  CoopNet_PeerName(id);  CoopNet_Ping(id);

CoopNet_Begin(type);  CoopNet_PutInt(v);  CoopNet_PutFloat(v);  CoopNet_PutString(v);
CoopNet_Send(to, reliable);          // to: peer id, COOPNET_HOST_ID (0) or COOPNET_ALL (-1)

int type = CoopNet_Recv();           // 0 = nothing; read fields in the order they were put:
CoopNet_From();  CoopNet_GetInt();  CoopNet_GetFloat();  CoopNet_GetString();
```

Types 1–5 are connection events (`COOPNET_EVT_*`), own messages use types from 100.
Clients talk only to the host; the host relays.

## Engine and script quirks

- A second `Maelstrom.exe` exits at once while a window titled "Pirates of the Maelstrom" exists.
  `run_pair.ps1` makes a local `Maelstrom_dev2.exe` with that title changed.
- A game window runs frames (and script events) only while it is active.
- Script identifiers are case-insensitive: `#define COOPNET_HOST` clashes with `CoopNet_Host()`.
- `default:` labels are global: `seadogs.c` already has one, a second one fails to compile.
- `ClearEvents()` (game load, teleport) drops posted events; the CoopNet tick must be restarted after it.
- Compile errors go to `error.log`; the failing function then misbehaves silently.

## Requirements

- DirectX 9 runtime from the Microsoft DirectX End-User Runtime (June 2010), x64:
  - `d3dx9_43.dll` — without it `Animation.dll`, `Sea.dll`, `Weather.dll` and `renderer/dx9.dll`
    fail with error 126 and the game exits with "No service: RENDER".
  - `D3DCompiler_43.dll` — loaded dynamically by d3dx9 to compile shaders. Without it `dx9.dll`
    crashes on a null pointer while compiling techniques and the game silently closes right after
    "Starting DirectX 9".

  Either install the full runtime (`directx_Jun2010_redist.exe` → `DXSETUP.exe`) or extract both
  x64 DLLs from its `Jun2010_d3dx9_43_x64.cab` / `Jun2010_D3DCompiler_43_x64.cab` next to `Maelstrom.exe`.

## Encoding

Scripts are Windows-1251 with CRLF line endings. `.gitattributes` disables any conversion;
edit them only with tools that preserve the encoding.
