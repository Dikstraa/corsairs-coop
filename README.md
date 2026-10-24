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

WorldMap.dll treats encounter attributes as output only: a ship reads its position and heading once,
when it is made (`WdmEnemyShip::SetSaveAttribute`), and gets a random heading
(`CreateMerchantShipXZ`: `Teleport(x, z, rand())`, speed clamped to at least 0.1). Steering one from
the script means making it again and again: a jump, a turn, and the old one fading out next to it
(the "copies everywhere"). So CoopNet.dll drives the ships itself (`CoopNet_MapShip*`):

- an avatar (and a mirror of another player's NPC ship) is created with a kSpeed no other ship has
  (0.1 + n * 0.00013), a goal far off and a long life; the DLL finds that float in memory and checks
  the WdmShip layout around it (`ax, ay, az, dltAx, dltAz, speed, turnspd, baseV, kMaxSpeed` in a
  row, the matrix row `x, 0, z, 1` a little before). Ships come from the same heap, so after the
  first find (~150 ms) the next ones take no time;
- every frame (`CoopNet_MapShipDrive` from the frame handler) it writes the position and heading
  there and keeps the ship's own speed and turning at 0. The position sails on with the reported
  speed and closes the gap to the report over ~0.5 s (jumps only beyond 30 units); the heading
  turns at 2 rad/s at most. Reads and writes are guarded, and a ship whose matrix or zeroed speed
  isn't as left is dropped, never written;
- a ship must be let go before the map frees it: every deletion goes through
  `CoopNet_DeleteEncounter`, which lets go first, and a ship whose record shows `needDelete` or
  `killMe` is let go too. When the DLL can't find a ship the old way (made anew when it drifts) is
  used.

Measured (`run_pair.ps1 -NewGame -ToMap`, the host sailing about with keys held through
`CoopNet_TestKey`): one avatar per player for the whole run instead of 17 re-creations in 80 s,
0.4-3 map units off, at most 0.08 units of correction in a frame; NPC mirrors no longer re-created. The owner reports its ships every 0.5 s with their real speed and heading
(measured on its map: they sail round islands, and the `speed` attribute is the engine's, before the
wind), and mirrors are carried forward with those: at most 0.27 units of correction in a frame.

Names: the DLL also draws each player's name under its ship (`map_labels.cpp`): the device's
EndScene, SetTransform and Reset are redirected (every device here has a table of its own, so the
game's table is found by the functions in it), the map camera is the one of the 3D scene (the map is
drawn around the camera, so the object nearest the middle of the view is taken as the player's own
ship and the others are placed from it), and the name is drawn with d3dx9_43.dll's font. A player
off the screen is shown at its edge with an arrow toward it; a player ashore or at sea keeps its ship
where it left the map, named "(на суше)" / "(в море)". A player whose ship isn't driven (not made yet, or the fallback merchant) still gets its
name where its report puts it (`CoopNet_MapMark`), so every player with a map position can be found.

Player list: **F3** (free in the stock controls; read by the DLL itself, so it works on every screen)
shows and hides a box with everybody in the session, where they are (map, sea, land with the town)
and their ping. All traffic goes through the host, so a client's ping to another client is its own
plus that one's to the host, which rides along in the presence message (`CoopNet_SetPanel`).

Running into an avatar doesn't start a battle (`wdmEvent_ShipEncounter`, `INTERFACE/map.c` skip it);
leftovers from an earlier visit are purged when the map opens.

A new encounter gets its `x`/`z` only when the map runs next, and a deleted one goes only when the
map runs next and still finds its `needDelete`: its attributes must stay (the map drops them itself
when it opens again, as the game does). While a dialog holds the map, an avatar waits instead of
being made again every tick (that once left dozens of copies of a player on the map).

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
  its latest save of this game a few seconds later, or at once when it is the host that died
  (`CoopNet_RollbackSave`: a save written since the game was started, else the one it was loaded
  from; never an older file from the folder).
- Whatever game the host starts or loads (also a save it picks itself), it tells everybody: the
  world is sent again and every client asks for its hero, which comes from that save
  (`CoopNetGuests` is saved with it). A client the save doesn't know yet starts a new game with the
  usual hero creation.
- While any other player boards (`bAbordageStarted`, sent with the presence), this player's game
  stands still (time scale 0, held every tick since menus reset it) until the boarding is over.
  The own cabin and deck are boarding locations too (`bCabinStarted`, `bDeckBoatStarted`) and
  don't count.
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
  Relations are one-way: the puppet itself sees everybody as a friend (its AI must not fight on
  its own: its shots come from its owner), so the battle interface, which colours a ship by its
  relation to the player, reads the puppet's side from `coopRel` instead (`SeaAI_GetRelation`):
  another player's pirates are red, not green.
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

## Players on land

`Program/coop/coop_land.c`: every player on land (a town, jungle, house; not at sea, so not in a
cabin, on a deck or boarding, and not in the hero selection room) sends its location, position,
heading, speed, drawn blade and look (model, animation, name) four times a second; the host passes
it on. Locations are the same for everybody, so positions are used as they come.

Another player in the same location is a puppet character `CoopLand_<id>` with that player's model:
actor type, immortal, no dialog. It runs (`Runto point`) or walks (`Goto point`) to where the player
will be half a second later (running whenever it is more than 2 m behind), is put in place when it
falls more than 6 m behind (plus 1.5 s of its speed when it runs, 15 m at most; a report faster than
15 m/s is a teleport, not a run), turns to the player's heading when it stands, and draws its blade
with the player. Measured: people walk 1.3-2.2 m behind; the only jumps left are the game's own
teleports of its people. A puppet is sent away
(`location = "none"`) when its player leaves or this player's location unloads, so the game never
logs it in again when it loads that location.

People: the stock characters (index below `GlobalCharacters`: merchants, governors, ...) are the
same in every game and stay local. Everybody else in a location (citizens, soldiers, bandits,
monsters, quest people) was made by the game that loaded it, so the player who has been there
longest owns the location (each report says how long; the lower id wins a tie). The owner sends
its people (`LAi_loginedcharacters`) twice a second, 6 per message (an unreliable message must fit
one UDP packet, ~1190 bytes; a bigger one is dropped); the others see them as
puppets `CoopLand_<owner>_<idx>` that walk, draw blades and die with them (`LAi_KillCharacter`, a
scripted death: nobody gets the kill). A player who is not the owner hides its own made-up people
(`location = "none"`, the old address kept in `coopLandHidden`) and gets them back when the owner
leaves or when it leaves itself. Officers are never hidden: every player sends its own.

Fights: a puppet has 100000 health and is immortal except during a hit. `LAi_CharacterAttack` and
`LAi_CharacterFire` (`loc_ai/LAi_events.c`) wrap the damage call with `CoopNet_LandHitBefore/After`:
the hit is worked out as usual (attacker's weapon, skills, perks, criticals, experience), the health
the puppet lost is put back and sent to its owner (`MSG_LAND_HIT`), whose game takes it off the
real character (`LAi_ApplyCharacterDamage` + `LAi_CheckKillCharacter`; the hero itself for a
player's puppet, so a player can die from another game's bandits, and that rolls the world back)
and sets it on the attacker's puppet there. Sides come with the people: foes (hostile to the
owner's hero, puppets in group `CoopFoes`, hostile to ours), allies (officers; `LAI_GROUP_PLAYER`)
and neutral (`CoopPeople`). Another player's puppet is an ally, or a foe when the two flags are at
war (PvP). So the owner's bandits go for every player's puppet, everybody's officers go for the
foes, and our own hits on allies don't count. Players' puppets share the player group: a crime by
one player turns the town on all of them.

Every person (and player) comes with the real one's rank, nation, town (`City`), type
(`GetCharType`) and health. A puppet is made with that rank (its skills, so how well it defends,
follow the rank) and nation, and shows the real health (outside a hit; the reserve is only put in
for the measurement). When a forwarded hit kills the real one, its owner tells the player who
dealt it (`MSG_LAND_KILL`), whose game gives the kill to whichever of its own hit last, as the
game itself does: skill experience (blade or pistol), kill statistics by the real type,
`LAi_SetResultOfDeath` (reputation, hunters, the town's nation turning hostile).

A hit carries who dealt it (the hero or which of its people) and the blow (`fast`, `force`,
`round`, `break`, `gun`): where it lands, that one's puppet turns to the target and plays the swing
(`attack_*`, `Shot`), and a person hit fights back against exactly that puppet. So a player's
officers fight in another player's location too: they are sent as allies, the owner's bandits
attack their puppets and the blows reach the real officers, while the real officers attack the
foes' puppets and get the kills.

Every swing and shot is shown too, hit or not: `ChrFgtActApply` (the engine's "a blow starts" event,
which takes its energy) and `Location_CharacterFire` send `MSG_LAND_SWING` for the hero and the people
we send, and their puppets play it at once and stand for 0.8 s (a walk task would cut the swing short);
a hit that follows only turns the puppet to its target.

`run_pair.ps1 -NewGame -TestLand`: both go to Bridgetown, the client 15 s after the host, so the
host owns the town; the client gets an officer for the test; one of the host's people strikes the
client's puppet, later its officer's puppet; the officer strikes one of the host's people; the client runs 10 m and
back, draws its blade and strikes one of the host's people 5 times; after 75 s the host goes to the
tavern. Measured: 3 blows of 23-27 killed the host's beggar there, later blows were dropped; the
host's guards then went for the client's puppet and their blows killed the client's hero. Measured: a running player puppet up to 3 m behind, a standing one 0.2 m; the
client hid 33-40 of its townsfolk and showed the host's 35-36, walking up to ~5 m behind (under
the 6 m jump); its own came back half a second after the host left.

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
- it stops while any player is in a battle (sea battle near enemies, boarding; a fight on land
  doesn't stop it, the others play on);
- otherwise it runs at the pace of the slowest player that needs time to pass. Each player's
  pace is measured from how fast its own engine moves time (world map, weather on land/sea,
  time acceleration); menus and dialogs don't hold the others back.

One game speed for everybody (`coop_rules.c`): the time keys (`TimeScaleCounter`, seadogs.c calls
`CoopNet_SpeedChanged`) change it for all players, the last change wins (`MSG_SPEED`, the host
repeats it every 5 s for players who came in later; for 1.5 s after its own change a player ignores
older words). A player's own pauses stay its own: the speed is put back only when no dialog,
interface (`InterfaceStates.Launched`) or boarding pause holds its game, as interfaces closing set
the counter back to 0. So everybody accelerates together, and the clock with them.

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
- A negative int from `CoopNet_GetInt` prints as negative but compares as unsigned (`-1 < 0` is
  false): send such values shifted up (`MSG_LAND_HIT` sends index + 1).
- `LAi_ApplyCharacterAttackDamage` skips immortals entirely, and the fight code reads
  `chr_ai.special` (made when a blade is equipped; `UnsetSpecialAttributes` makes it for a puppet).

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
