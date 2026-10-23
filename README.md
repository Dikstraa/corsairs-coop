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

The repository carries everything a player needs on top of an installed Ship Pack: the scripts, the
built `modules/CoopNet.dll`, `steam_api64.dll` (redistributable from the Steamworks SDK) and
`coop.ini` (`mode=auto`, `transport=steam`). Clone or copy it over the game folder, start Steam, start
the game: the first player hosts, the next ones join a Steam friend's game by themselves (or through a
Steam invite). Everybody must run the same version.

Developers: `CoopMod/build.bat` rebuilds `CoopNet.dll` (VS 2022 Build Tools, x64; Steam support needs
the Steamworks SDK unpacked in `CoopMod/steamworks_sdk/`, not in git) and copies it to `modules/`;
commit the rebuilt DLL with the source change. Other `coop.ini` settings: `mode=host|join|off`,
`transport=udp` with the host's IP in `address` for LAN/VPN, `name=` to override the Steam name.

Steam P2P goes through Valve relays: no port forwarding. Two instances on one PC share a Steam
account and can't connect to each other over Steam; test those over UDP (`run_pair.ps1`).

With `transport=steam` the DLL starts the Steam API (as Spacewar, `SteamAppId`/`SteamGameId` set to 480)
as soon as the engine loads its modules, before the renderer creates its Direct3D device; that is
what lets the Steam overlay (Shift+Tab) hook the game even when it isn't started from Steam.
`mode=auto` (the default in `coop.ini`) joins a friend's game if there is one, else hosts;
an empty host accepts a friend's invite and moves to the friend's game. In-game messages
("Кооператив: ...") report connections.

The Esc menu has a co-op button (`RESOURCE/INI/interfaces/game_menu.ini`, label `CoopInvite` in
`RESOURCE/INI/TEXTS/russian/common.ini`): outside a session the first press creates a game, then it
opens the Steam overlay's invite dialog. The script tick runs from the `frame` event timed by real
time, since posted events stop while the Esc menu pauses the game.

## Players on the world map

`Program/coop/coop_presence.c`: every player sends the host its mode, world map position and place
(location id) twice a second; the host sends the whole table to everybody. On the world map each
other player is shown as a ship (a merchant-type encounter marked `coopAvatar`).

WorldMap.dll treats encounter attributes as output only: writing `x`/`gotox` changes nothing, and an
encounter can only be created (`MSG_WORLDMAP_CREATEENC_MER_XZ`) or deleted (`needDelete` +
`worldMap.deleteUpdate = ""`). So an avatar is created slightly ahead of the player along its course
(encounters start from a standstill) and re-created when it drifts more than 20 map units or the
course turns by more than ~35°. A parked player is a ship crawling at kSpeed 0.0001. Running into
an avatar doesn't start a battle (`wdmEvent_ShipEncounter`, `INTERFACE/map.c` skip it); leftovers
from an earlier visit are purged when the map opens.

The world map keeps its own clock and ignores the time written back to it, so in a session its
speed (`worldMap.date.hourPerSec`) is steered: the shared pace plus a correction that closes the
gap to the shared clock within ~10 s. Its natural pace is `pchar.wmtimechange` (default 0.5 h/s).

## Shared world

`Program/coop/coop_world.c`: the host owns the world, every player keeps its own hero and quests.
Shared so far: store goods (quantity, price drift, trade type), colony owners and the relations
between nations. Personal: the Caiman colony, its warehouse and the ship-to-ship store.

- On every game start (new game or load) a client asks the host for its whole world and takes it
  as is; its own changes are only sent after that.
- Each player keeps a text snapshot per entry (`s<store>`, `c<colony>`, `r`) and checks a few
  entries per tick. An entry that no longer matches its snapshot was changed here (trade, capture,
  the host's daily update) and goes out: from a client to the host, from the host to everybody.
  The host applies a client's change and passes it on, so the last change wins everywhere.
- Clients skip the daily store update (`UpdateStore`, `ChangeImport`): it comes from the host.
- A changed colony owner is applied with `SetCaptureTownByNation` (mayor, garrison, map flag), keeping
  the player's deposits. A colony a player takes for itself shows up for the others as a pirate one.

`run_pair.ps1 -NewGame -TestWorld`: the host changes a store and a colony owner, the client another
store; both log what they see.

## Players' heroes kept by the host

`Program/coop/coop_heroes.c`: the host keeps every player's hero, so a returning player goes on
where it left.

- A player is known by its Steam id (`CoopNet_MyUid`: `steam<SteamID64>`), else by its name.
- While in the host's world a client sends the host its hero pack every minute: the hero (`pchar`),
  the characters in its `Fellows` lists (companions, officers, passengers), their `RealShips` entries
  and its place (land location and locator, else its world map position). Not while the new game
  still sits in the hero selection cabin.
- The host keeps the latest pack per player in `CoopNetGuests`, a global object: it is saved with the
  host's game.
- On connecting a client asks whether the host keeps its hero. A newcomer creates its hero as usual.
  A returning player in the main menu starts a game by itself and skips the hero creation; in a game
  (new or loaded) the kept hero replaces the local one once the host's world is in: characters are
  found by id or created, ships get new `RealShips` slots, `Ship.Type` and `Fellows` indices are
  remapped, then the hero is reloaded to its location (or put on the world map where it was).
- Trees travel as `(depth, name, value)` entries in preorder, 300 per message; the receiver keeps
  the names of the current path and walks down from the root for every entry.

Quest progress lives mostly in the hero's attributes and comes back with it; quest state kept in
other characters or global objects of the client's own game does not.

`run_pair.ps1 -NewGame -TestHero`: the client goes to Bridgetown, gets 12345 gold, is restarted and
comes back with them there.

## Death, rollback and boarding

`Program/coop/coop_rules.c`:

- A player's death rolls the whole world back. `GameOver` (seadogs.c) asks `CoopNet_GameOver` first:
  a client tells the host and goes through the usual death screen to the main menu; the host loads
  its latest save (`CoopNet_NewestFile` on its save folder) a few seconds later, or at once when it
  is the host that died.
- Whatever game the host starts or loads (also a save it picks itself), it tells everybody: the
  world is sent again and every client asks for its hero, which comes from that save
  (`CoopNetGuests` is saved with it). A client the save doesn't know yet starts a new game with the
  usual hero creation.
- While any other player boards (`bAbordageStarted`, sent with the presence), this player's game
  stands still (time scale 0, held every tick since menus reset it) until the boarding is over.
- A puppet taken by boarding (`ShipTaken`, `ShipTakenFree`, or sunk by `ShipDead` here) is reported
  to its own machine, which loses that ship with this player as its killer; the taken character
  stops being a puppet here.
- The script tick is re-registered at every game start and when the main menu opens, since loads
  and the game over clear the event handlers.

`run_pair.ps1 -NewGame -TestRules`: the host saves once it keeps the client's hero, the client
"boards" for ~10 s (the host pauses), then dies; the host loads its save and the client comes back
with the hero that save keeps.

## Shared ships on the world map

`Program/coop/coop_encounters.c`: the random ships of the world map (merchants, patrols, pirates,
warring ships, floating barrels) are the same for everybody. Quest encounters and storms stay personal.

- The game creates them around the player's own ship, so each player still generates the ones
  around itself (their owner), but registers every one with the host, which keeps the one list
  and passes it to everybody (a joining client gets it with the host's world).
- On the map the others' entries are shown as mirrors: encounters created at the owner's
  position (a little ahead, they start from a standstill), sailing to the same goal at the same
  speed, carrying the owner's encounter data, so attacking a mirror gives the same kind of
  squadron. The owner reports positions every 2 s while on the map; a mirror more than 30 map
  units off, or with a changed goal, is re-created (in tests every 10–20 s). While the owner is
  off the map its ships stand still for it, and so do the mirrors.
- A ship that disappears for anybody (taken into a battle, arrived, expired) is deleted for
  everybody. The generation limit counts mirrors too, so players sailing together don't double
  the traffic.
- A new world on the host (new game, load) makes everybody forget the list.

## Players at sea

`Program/coop/coop_sea.c`: players at sea in the same place share their sea battles.

- Every player at sea sends all ships of its scene four times a second: its own fleet (flagship and
  companions, whose officers and skills count on its machine) and the NPC ships of its scene, each
  with position, heading, speed, type, nation (flag) and hull; the host passes the list on. Near an
  island positions are in the island's sea coordinates, the same for everybody there. In open sea
  each scene is centred where its player left the world map, so positions travel as world map
  coordinates and are turned back into the receiver's own.
- Every ship is run by the machine it came from. Elsewhere, within 2500 sea units, it is a "puppet":
  an NPC with a ship of the same type that sails (AI move task) for a point ahead of the real ship.
- Sides follow the flags: another player's fleet is friend or enemy by the relation between its
  nation and this player's flag (France against England at war: the players fight each other); an
  NPC ship by its nation's relation to this player. Sides are re-applied when flags or relations
  change (`SetCharacterRelationBoth` for the ships this machine runs, then `UpdateRelations`).
- A puppet's top speed is the real ship's speed plus a share of the distance it is behind (`coopSpeed`, applied in `Ship_UpdateParameters`),
  so it catches up without overshooting: in tests it stays 7–25 sea units off.
- Ships in a running sea scene can't be moved or removed (`Group_SetXZ_AY` and writes to `Ship.Pos`
  are ignored), so a puppet is created where its ship is, and when the ship leaves the scene it
  sails away and comes back when the player does.

Hits and sinkings (`CoopNet_SeaHullHit` at the top of `Ship_ApplyHullHitpoints(WithCannon)`,
`CoopNet_SeaShipDead` at the top of `ShipDead`):

- A hit on a puppet by a ship this machine runs is not applied here: it goes to the puppet's own
  machine with this side's gunnery perks already in the damage, and that machine applies it with
  its ship's defences and remembers who hit last. Hits by puppets are only for show; the shooter's
  machine sends the real ones. Puppets show the hull their own machine reports.
- A ship that sinks is reported to everybody with whoever finished it. The puppet then sinks through
  `ShipDead` with the killer's ship on that machine: the real ship on the killer's machine, so the
  kill, experience and statistics go to the player who finished it, as in the single-player game.
- Crew losses on a puppet go to its machine the same way (`CoopNet_SeaCrewHit` at the top of
  `Ship_ApplyCrewHitpoints(WithCannon)`; the shooter comes from `Ship_HullHitEvent`, which calls
  `CoopNet_SeaBallHit` first). Puppets show the crew their machine reports. Sails are not shared:
  `Ship.SP` is worked out by the engine from the sails themselves, writes to it don't hold.
- Every ball a ship fires (`CoopNet_SeaBallFired` at the end of `Ball_AddBall`, after the shooter's
  dispersion) is queued and sent once per tick; elsewhere the same ball flies from the puppet
  (shifted by the puppet's offset from the real ship), with smoke and sound. Being a puppet's ball,
  its hits are only for show.
- Puppets make no decisions of their own (`Ship_CheckSituation` skips them), turn at any speed and
  slow down to turn when their goal is off the bow.

Joining from the world map: a player at sea within 5 map units (its open sea position, or where it
left the map near an island) is offered in the game log; **F9** (or sailing into its avatar) loads
this player into that scene, 200 sea units abeam of the other ship. `wdmReloadToSea` then takes the
scene's frame (`worldMap.island`, `zeroX/Z`, sent with every sea report) from `CoopNet_SeaJoinLogin`
instead of collecting the map's encounters. The world map only reports a contact with an encounter
when the ships close in under way, so standing next to an avatar alone doesn't trigger it.

`run_pair.ps1 -NewGame -ToSea`: both instances go from the world map to the sea, the host sets sail
and ~40 s later returns to the map. `-SeaJoin`: only the host goes; the client joins it from the map.

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
- Results through `ref` float parameters are unreliable: the callee's own locals end up in them.
  Return such results through attributes.

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
