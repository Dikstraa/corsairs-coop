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

## Playing

1. Build (`CoopMod/build.bat`) with the Steamworks SDK unpacked in `CoopMod/steamworks_sdk/`
   (not in git). It copies `CoopNet.dll` to `modules/` and `steam_api64.dll` next to `Maelstrom.exe`.
2. Copy `CoopMod/coop.ini.example` next to `Maelstrom.exe` as `coop.ini`: the host sets `mode=host`,
   the others `mode=join`. With `transport=steam` both need Steam running; the game uses
   Spacewar (AppID 480) and writes `steam_appid.txt` itself.
3. Steam: the client finds a lobby hosted by a Steam friend (else any open one), or by part of the
   host's name in `address`. While the game runs, accepting a Steam invite to the host's lobby joins too.

Steam P2P goes through Valve relays: no port forwarding. Two instances on one PC share a Steam
account and can't connect to each other over Steam; test those over UDP (`run_pair.ps1`).

## Script API

```c
CoopNet_Host(port, maxPeers, name);  CoopNet_Connect(host, port, name);  CoopNet_Disconnect();
CoopNet_State();  CoopNet_MyId();  CoopNet_PeerCount();  CoopNet_PeerId(i);  CoopNet_PeerName(id);  CoopNet_Ping(id);

CoopNet_Begin(type);  CoopNet_PutInt(v);  CoopNet_PutFloat(v);  CoopNet_PutString(v);
CoopNet_Send(to, reliable);          // to: peer id, COOPNET_HOST_ID (0) or COOPNET_ALL (-1)

int type = CoopNet_Recv();           // 0 = nothing; read fields in the order they were put:
CoopNet_From();  CoopNet_GetInt();  CoopNet_GetFloat();  CoopNet_GetString();
```

Types 1–5 are connection events (`COOPNET_EVT_*`), own messages use types from 100;
10–99 are handled inside the DLL. Clients talk only to the host; the host relays.

## Shared time

One clock for everybody, owned by the host (`coop_clock.h`):

- it starts from the host's world time after a new game or a loaded save;
- it stops while any player is in a battle (sea battle near enemies, boarding, land alarm);
- otherwise it runs at the pace of the slowest player that needs time to pass. Each player's
  pace is measured from how fast its own engine moves time (world map, weather on land/sea,
  time acceleration), so acceleration only works when everybody accelerates;
  menus and dialogs don't hold the others back.

Hooks: `Whr_TimeUpdate` (WhrWeather.c) and `wdmEvent_UpdateDate` (worldmap_events.c) pass the
engine's proposed time through `CoopNet_TimeHook`, which applies the shared date and time.
During a session the DLL also keeps the game running when its window is not active.

Time skips are voted (`coop_vote.h`): `AddTimeToCurrent`, `AddDataToCurrent` (calendar.c) and
`WaitDate` (quests.c) call `CoopNet_InterceptSkip` first. In a session the skip becomes a request to
the host; everybody else sees it in the game log and agrees with **F8** within 30 s. When all agree
the host moves the clock and everyone jumps together; one "no" or a timeout rejects it. Skips under
one game hour need no vote. A quest waiting on `WaitDate` continues for the requester either way,
so it can't get stuck. `SetCurrentTime` is not intercepted (the weather calls it on every location
load); in a session the shared clock simply overrides it.

Test without clicking through menus: `run_pair.ps1 -NewGame` starts both instances into a new game;
`-TestSkip` also makes the host sleep 8 hours and the client agree.

## Engine and script quirks

- A second `Maelstrom.exe` exits at once while a window titled "Pirates of the Maelstrom" exists.
  `run_pair.ps1` makes a local `Maelstrom_dev2.exe` with that title changed.
- A game window runs frames (and script events) only while it is active; the engine learns that
  from WM_ACTIVATE/WM_ACTIVATEAPP only, which CoopNet.dll hides during a session.
- `loadedLocation` is an unset ref in the main menu (`IsEntity(loadedLocation)` is a runtime error
  there), and `bLandInterfaceStart` stays false in the hero selection cabin: location presence is
  tracked with `EVENT_LOCATION_LOAD/UNLOAD` handlers.
- Files compiled before `coop/coopnet.c` can call its functions but can't use its `#define`s.
- The weather reports time only every few game minutes, so the clock is applied in steps.
- Script identifiers are case-insensitive: `#define COOPNET_HOST` clashes with `CoopNet_Host()`.
- A condition can't start with a parenthesized group: `if ((a || b) && c)` fails to compile.
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
