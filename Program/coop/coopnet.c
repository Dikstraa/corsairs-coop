// CoopNet: co-op network layer (modules/CoopNet.dll), sources in CoopMod/src

// CoopNet_State()
#define COOPNET_OFFLINE			0
#define COOPNET_CONNECTING		1
#define COOPNET_CONNECTED		2
#define COOPNET_HOSTING			3

// peer ids for CoopNet_Send
#define COOPNET_HOST_ID		0	// not COOPNET_HOST: names are case-insensitive, it would clash with CoopNet_Host()
#define COOPNET_ALL				-1

// reliable flag for CoopNet_Send
#define COOPNET_UNRELIABLE		0
#define COOPNET_RELIABLE		1

// message types produced by the DLL; own messages start at COOPNET_MSG_USER
#define COOPNET_EVT_CONNECTED		1	// int myId
#define COOPNET_EVT_CONNECT_FAILED	2	// int reason
#define COOPNET_EVT_PEER_JOINED		3	// int peerId, string name
#define COOPNET_EVT_PEER_LEFT		4	// int peerId, int reason
#define COOPNET_EVT_DISCONNECTED	5	// int reason
#define COOPNET_EVT_SKIP_VOTE		6	// string text: somebody asks to skip time (F8 agrees)
#define COOPNET_EVT_SKIP_RESULT		7	// int approved, int mine, string continuation, string text
#define COOPNET_EVT_NOTICE			8	// string text
#define COOPNET_MSG_USER			100

#define COOPNET_MSG_TEST_HELLO		100	// client -> host: string text, float f, int i
#define COOPNET_MSG_TEST_ECHO		101	// host -> client: same fields + string hostName

#define COOPNET_DEFAULT_PORT	47600
#define COOPNET_TICK_MS			50

// CoopNet_SetStatus modes
#define COOPNET_MODE_MENU		0
#define COOPNET_MODE_WORLDMAP	1
#define COOPNET_MODE_SEA		2
#define COOPNET_MODE_LAND		3

// CoopNet_ClockEngine sources
#define COOPNET_CLOCK_WORLDMAP	1
#define COOPNET_CLOCK_WEATHER	2

native int CoopNet_Host(int port, int maxPeers, string name);
native int CoopNet_Connect(string host, int port, string name);
native int CoopNet_Disconnect();
native int CoopNet_State();
native int CoopNet_MyId();
native int CoopNet_TestKey(int vk, int down);
native int CoopNet_MapShipBind(string enc, float kSpeed, float x, float z);
native int CoopNet_MapShipSet(string enc, float x, float z, float vx, float vz, float ay);
native int CoopNet_MapShipDrive(float meX, float meZ);
native int CoopNet_MapShipForget(string enc);
native int CoopNet_MapShipLabel(string enc, string text);
native int CoopNet_MapMark(int id, float x, float z, string text);
native int CoopNet_SetPanel(string text);
native int CoopNet_PeerCount();
native int CoopNet_PeerId(int index);
native string CoopNet_PeerName(int id);
native int CoopNet_Ping(int id);
native string CoopNet_LastError();
native int CoopNet_SetLoss(float fraction);
native int CoopNet_Begin(int type);
native int CoopNet_PutInt(int v);
native int CoopNet_PutFloat(float v);
native int CoopNet_PutString(string v);
native int CoopNet_Send(int to, int reliable);
native int CoopNet_Recv();
native int CoopNet_From();
native int CoopNet_GetInt();
native float CoopNet_GetFloat();
native string CoopNet_GetString();
native int CoopNet_Version();
native int CoopNet_Log(string s);
native string CoopNet_GetEnv(string name);
native int CoopNet_Update();
native int CoopNet_TickAge();
native int CoopNet_SetStatus(int mode, int battle);
native float CoopNet_ClockEngine(int year, int month, int day, float hours, int source);
native int CoopNet_ClockYear();
native int CoopNet_ClockMonth();
native int CoopNet_ClockDay();
native int CoopNet_ClockDaysAhead();
native float CoopNet_ClockRate();
native int CoopNet_ClockRestart();
native int CoopNet_SkipVoting();
native int CoopNet_RequestSkip(int minutes, string continuation);
native int CoopNet_VoteYes();
native int CoopNet_UseSteam();
native int CoopNet_UseUdp();
native string CoopNet_Config(string key);
native int CoopNet_SetLocalRate(float gameSecondsPerSecond);
native float CoopNet_Seconds();
native int CoopNet_Invite();
native string CoopNet_MyUid();
native string CoopNet_RollbackSave(string dir);
native int CoopNet_SaveLoaded(string path);

#library "COOPNET"


bool bCoopNetTicking = false;
bool bCoopNetSteam = false; // Steam transport: keep ticking while offline so accepted invites are noticed
int iCoopNetMode = 0;       // this player's COOPNET_MODE_*, set by CoopNet_UpdateStatus

void CoopNet_Init()
{
	CoopNet_Log("script layer up, dll v" + CoopNet_Version());

	// Test instances: start the game with COOPNET_TEST=host or COOPNET_TEST=client
	string sTest = CoopNet_GetEnv("COOPNET_TEST");
	if (sTest == "host")
	{
		if (CoopNet_Host(COOPNET_DEFAULT_PORT, 4, "TestHost")) CoopNet_StartTick();
		else CoopNet_Log("host failed: " + CoopNet_LastError());
	}
	if (sTest == "client")
	{
		if (CoopNet_Connect("127.0.0.1", COOPNET_DEFAULT_PORT, "TestClient")) CoopNet_StartTick();
		else CoopNet_Log("connect failed: " + CoopNet_LastError());
	}
	if (sTest == "steamhost" || sTest == "steamjoin")
	{
		bCoopNetSteam = CoopNet_UseSteam();
		if (!bCoopNetSteam) CoopNet_Log("steam unavailable: " + CoopNet_LastError());
		if (bCoopNetSteam && sTest == "steamhost") CoopNet_Host(0, 4, "");
		if (bCoopNetSteam && sTest == "steamjoin") CoopNet_Connect("", 0, "");
		CoopNet_StartTick();
	}
	if (sTest == "") CoopNet_StartFromConfig();
}

// mode=auto hosts when no friend's game is found.
bool bCoopNetAutoHost = false;
int iCoopNetAutoPort = COOPNET_DEFAULT_PORT;
int iCoopNetAutoMax = 4;
string sCoopNetAutoName = "";

// coop.ini next to Maelstrom.exe:
//   [coop]
//   mode=host | join | auto | off   (auto: join a friend's game if there is one, else host)
//   transport=steam | udp
//   address=   udp: host IP; steam: empty = a friend's game, part of the host's name, or lobby:<id>
//   port=47600   (udp)
//   maxplayers=4 (host)
//   name=        empty = Steam name
void CoopNet_StartFromConfig()
{
	string sMode = CoopNet_Config("mode");
	string sTransport = CoopNet_Config("transport");
	if (sTransport == "steam")
	{
		if (!CoopNet_UseSteam())
		{
			CoopNet_Log("steam unavailable: " + CoopNet_LastError());
			return;
		}
		bCoopNetSteam = true;
		CoopNet_StartTick();
	}
	if (sMode != "host" && sMode != "join" && sMode != "auto") return;

	int iPort = COOPNET_DEFAULT_PORT;
	string sValue = CoopNet_Config("port");
	if (sValue != "") iPort = sti(sValue);
	int iMax = 4;
	sValue = CoopNet_Config("maxplayers");
	if (sValue != "") iMax = sti(sValue);
	string sName = CoopNet_Config("name");
	string sAddress = CoopNet_Config("address");
	if (!bCoopNetSteam && sAddress == "") sAddress = "127.0.0.1";

	iCoopNetAutoPort = iPort;
	iCoopNetAutoMax = iMax;
	sCoopNetAutoName = sName;
	if (sMode == "auto") bCoopNetAutoHost = true;
	bool bOk;
	if (sMode == "host") bOk = CoopNet_StartHosting();
	else bOk = CoopNet_Connect(sAddress, iPort, sName);
	if (bOk) CoopNet_StartTick();
	else CoopNet_Log(sMode + " failed: " + CoopNet_LastError());
}


bool CoopNet_StartHosting()
{
	bCoopNetAutoHost = false;
	if (iCoopNetAutoPort <= 0) iCoopNetAutoPort = COOPNET_DEFAULT_PORT; // global initializers don't take #defines
	if (iCoopNetAutoMax <= 0) iCoopNetAutoMax = 4;
	if (!CoopNet_Host(iCoopNetAutoPort, iCoopNetAutoMax, sCoopNetAutoName)) return false;
	CoopNet_Notice("Кооператив: игра создана, друзья могут подключиться");
	return true;
}

// From the main menu's start (INTERFACE/mainmenu.c): GameOver cleared the event handlers on the way.
void CoopNet_MainMenuShown()
{
	if (bCoopNetTicking) CoopNet_StartTick();
}

// The co-op button of the Esc menu (INTERFACE/game_menu.c): opens the Steam overlay's invite dialog.
// Not in a session yet: the first press creates the game (a Steam lobby), the next one invites.
void CoopNet_MenuInvite()
{
	CoopNet_Log("menu: co-op button, state " + CoopNet_State() + ", steam " + bCoopNetSteam);
	if (!bCoopNetSteam)
	{
		bCoopNetSteam = CoopNet_UseSteam();
		if (!bCoopNetSteam)
		{
			CoopNet_Notice("Кооператив: Steam недоступен - запустите Steam");
			return;
		}
		CoopNet_StartTick();
	}
	if (CoopNet_State() == COOPNET_OFFLINE)
	{
		if (CoopNet_StartHosting())
		{
			CoopNet_StartTick();
			CoopNet_Notice("Кооператив: нажмите кнопку ещё раз, чтобы пригласить друга");
		}
		return;
	}
	if (!CoopNet_Invite()) CoopNet_Notice("Кооператив: откройте Steam (Shift+Tab), чтобы пригласить друга");
}
// A message on the game screen and in coopnet.log.
void CoopNet_Notice(string sText)
{
	CoopNet_Log("notice: " + sText);
	Log_Info(sText);
}

// ---- test helpers ------------------------------------------------------------

int iCoopNetTestMenuSeen = 0;

// COOPNET_NEWGAME=1: presses "New game" once the main menu has been up for ~3 s. Runs from the
// tick (a separately posted event doesn't survive the menu start's ClearEvents).
void CoopNet_TestNewGame()
{
	if (iCoopNetTestMenuSeen < 0 || CoopNet_GetEnv("COOPNET_NEWGAME") != "1") return;
	if (CurrentInterface != INTERFACE_MAINMENU) return;
	iCoopNetTestMenuSeen++;
	if (iCoopNetTestMenuSeen < 60) return;
	iCoopNetTestMenuSeen = -1;
	CoopNet_Log("test: starting a new game");
	// what IDoExit in INTERFACE/mainmenu.c does
	InterfaceStates.BackEnvironmentIsCreated = true;
	DelEventHandler("backgroundcommand", "ProcCommand");
	interfaceResultCommand = RC_INTERFACE_DO_NEW_GAME;
	EndCancelInterface(true);
}

int iCoopNetClockLogTicks = 0;
int iCoopNetTestSkipTicks = 0;
int iCoopNetTestMapTicks = 0;

// Logs an attribute tree (two levels) for exploring engine objects.
void CoopNet_LogTree(aref arRoot, string sPrefix)
{
	int iNum = GetAttributesNum(arRoot);
	for (int i = 0; i < iNum; i++)
	{
		aref arItem = GetAttributeN(arRoot, i);
		string sName = GetAttributeName(arItem);
		CoopNet_Log(sPrefix + sName + " = " + GetAttributeValue(arItem));
		int iSub = GetAttributesNum(arItem);
		for (int j = 0; j < iSub; j++)
		{
			aref arSub = GetAttributeN(arItem, j);
			CoopNet_Log(sPrefix + "  " + GetAttributeName(arSub) + " = " + GetAttributeValue(arSub));
		}
	}
}

// COOPNET_TOMAP=1: goes to the world map once in the game; the host then sails about after ~25 s (keys held through
// CoopNet_TestKey: the map ignores position writes) so the client's avatar of it has something to follow.
void CoopNet_TestMap()
{
	if (iCoopNetTestMapTicks < 0 || CoopNet_GetEnv("COOPNET_TOMAP") != "1") return;
	if (!CoopNet_SkipVoting()) return;
	iCoopNetTestMapTicks++;
	if (iCoopNetTestMapTicks == 80 && CurrentInterface == INTERFACE_CREATE_CHARACTER) Event("InterfaceBreak"); // a player closes it with Ok
	if (iCoopNetTestMapTicks == 100 && !IsEntity(&worldMap))
	{
		if (!CheckAttribute(pchar, "Ship.Pos.x")) { pchar.Ship.Pos.x = 0.0; pchar.Ship.Pos.z = 0.0; }
		if (!CheckAttribute(pchar, "Ship.Ang.y")) pchar.Ship.Ang.y = 0.0; // the hero selection cabin has no ship angle
		CoopNet_Log("test: Land_MapLoad from " + pchar.location);
		Land_MapLoad();
		return;
	}
	if (iCoopNetTestMapTicks == 700 && IsEntity(&worldMap) && CoopNet_State() == COOPNET_CONNECTED)
	{
		// what going into a battle does to the ships it takes along
		aref arEncs;
		makearef(arEncs, worldMap.encounters);
		int iNum = GetAttributesNum(arEncs);
		for (int i = 0; i < iNum; i++)
		{
			aref arEnc = GetAttributeN(arEncs, i);
			if (!CheckAttribute(arEnc, "coopMirror")) continue;
			CoopNet_Log("test: client takes " + GetAttributeName(arEnc) + " (" + arEnc.coopGid + ") into a battle");
			arEnc.needDelete = "Reload delete non quest encounter";
			worldMap.deleteUpdate = "";
			break;
		}
	}
	if (CoopNet_GetEnv("COOPNET_TOSEA") == "1") CoopNet_TestSea();
	if (!IsEntity(&worldMap) || CoopNet_State() != COOPNET_HOSTING) return;
	if (CoopNet_GetEnv("COOPNET_TOSEA") == "1") return;
	// the host sails about as a player would: sails up (W held 2 s), then a turn to the left (A held 1 s)
	// every 10 s, so the client's avatar of it has a moving, turning ship to follow
	int iT = iCoopNetTestMapTicks;
	if (iT == 300)
	{
		// the host speeds the game up: the client follows
		TimeScaleCounter = 4;
		SetTimeScale(2.0);
		CoopNet_SpeedChanged();
		CoopNet_Log("test: host speeds the game up to x2");
	}
	if (iT == 600)
	{
		CoopNet_TestKey(87, 1);
		CoopNet_Log("test: host sets sail on the map at " + worldMap.playerShipX + " " + worldMap.playerShipZ);
	}
	if (iT == 640) CoopNet_TestKey(87, 0);
	int iPhase = iT - (iT / 200) * 200;
	if (iT > 650 && iPhase == 0) CoopNet_TestKey(65, 1);
	if (iT > 650 && iPhase == 20)
	{
		CoopNet_TestKey(65, 0);
		CoopNet_Log("test: host turned, now at " + worldMap.playerShipX + " " + worldMap.playerShipZ + " heading " + worldMap.playerShipAY);
	}
}

// COOPNET_TOSEA=1 (with COOPNET_TOMAP): ~10 s on the map both leave it for the sea (with COOPNET_SEAJOIN=1 only\r\n// the host; the client stays by the host's avatar and so joins it at sea), the client 2 map units
// (100 sea units) east of the host; ~10 s later the host sets full sails, ~40 s later it goes back to\r\n// the map.
void CoopNet_TestSea()
{
	if (iCoopNetTestMapTicks == 300 && IsEntity(&worldMap))
	{
		if (CoopNet_GetEnv("COOPNET_SEAJOIN") == "1" && CoopNet_State() == COOPNET_CONNECTED) return; // waits on the map by the host's avatar
		if (CoopNet_State() == COOPNET_CONNECTED) worldMap.playerShipX = stf(worldMap.playerShipX) + 2.0;
		if (CoopNet_GetEnv("COOPNET_SEAFLAGS") == "1") CoopNet_TestFlags();		CoopNet_Log("test: to the sea from map x " + worldMap.playerShipX);
		wdmReloadToSea();
	}
	if (bSeaActive && CoopNet_GetEnv("COOPNET_SEAFLAGS") == "1")
	{
		if (iCoopNetTestMapTicks == 650 && CoopNet_State() == COOPNET_CONNECTED)
		{
			CoopNet_Log("test: client fires a broadside");
			Ship_DoFire();
		}
		if (iCoopNetTestMapTicks == 700 && CoopNet_State() == COOPNET_CONNECTED) CoopNet_TestCrewSails();
		if (iCoopNetTestMapTicks == 750 && CoopNet_State() == COOPNET_HOSTING) CoopNet_TestAttack();
		if (iCoopNetTestMapTicks == 800 && CoopNet_State() == COOPNET_CONNECTED) CoopNet_TestAttack();
	}
	if (false)
	{
		CoopNet_TestAttack();
		return;
	}
	if (iCoopNetTestMapTicks == 1100 && CoopNet_GetEnv("COOPNET_SEAFLAGS") == "1") return;
	if (iCoopNetTestMapTicks == 550 && bSeaActive && CoopNet_State() == COOPNET_HOSTING)
	{
		CoopNet_Log("test: host sets full sails");
		Ship_SetSailState(nMainCharacterIndex, 1.0);
	}
	if (iCoopNetTestMapTicks == 1100 && bSeaActive && CoopNet_State() == COOPNET_HOSTING)
	{
		CoopNet_Log("test: host goes back to the map");
		Sea_MapLoad();
	}
}

// COOPNET_SEAFLAGS=1: before going to sea France and England go to war, the host hoists the French flag,
// the client the English one and gets a companion of its own.
void CoopNet_TestFlags()
{
	if (CoopNet_State() == COOPNET_HOSTING)
	{
		pchar.nation = FRANCE;
		SetNationRelationBoth(FRANCE, ENGLAND, RELATION_ENEMY); // reaches the client with the shared world
		CoopNet_Log("test: host sails under France, relation to England " + GetRelationName(GetNationRelation(FRANCE, ENGLAND)));
		return;
	}
	pchar.nation = ENGLAND;
	int iMate = NPC_GenerateCharacter("CoopTestMate", "officer_15", "man", "man", 10, ENGLAND, -1, true);
	ref rMate = &Characters[iMate];
	rMate.Ship.Type = GenerateShipExt(sti(RealShips[sti(pchar.Ship.Type)].BaseType), true, rMate);
	SetBaseShipData(rMate);
	SetCrewQuantityFull(rMate);
	SetCompanionIndex(pchar, -1, iMate);
	CoopNet_Log("test: client sails under England with companion " + rMate.id);
}
// COOPNET_SEAFLAGS=1 at sea: the client fires a broadside, the host's hull goes down to 20%, then a ball
// of the client's companion hits the host's flagship for 500.
void CoopNet_TestAttack()
{
	if (CoopNet_State() == COOPNET_HOSTING)
	{
		pchar.Ship.HP = 0.2 * stf(RealShips[sti(pchar.Ship.Type)].HP);
		CoopNet_Log("test: host hull down to " + pchar.Ship.HP);
		return;
	}
	int iMate = GetCharacterIndex("CoopTestMate");
	if (!CheckAttribute(&CoopNetSea, "p0.ships")) return;
	aref arShips;
	makearef(arShips, CoopNetSea.p0.ships);
	int iNum = GetAttributesNum(arShips);
	for (int i = 0; i < iNum; i++)
	{
		aref arS = GetAttributeN(arShips, i);
		if (sti(arS.flags) != COOPNET_SEA_FLAGSHIP || !sti(arS.inSea)) continue;
		CoopNet_Log("test: companion hits " + Characters[sti(arS.chr)].id + " for 500");
		Ship_ApplyHullHitpoints(&Characters[sti(arS.chr)], 500.0, KILL_BY_BALL, iMate);
	}
}
// COOPNET_SEAFLAGS=1 at sea: the client's companion kills 10 of the host's crew.
void CoopNet_TestCrewSails()
{
	if (!CheckAttribute(&CoopNetSea, "p0.ships")) return;
	aref arShips;
	makearef(arShips, CoopNetSea.p0.ships);
	int iNum = GetAttributesNum(arShips);
	for (int i = 0; i < iNum; i++)
	{
		aref arS = GetAttributeN(arShips, i);
		if (sti(arS.flags) != COOPNET_SEA_FLAGSHIP || !sti(arS.inSea)) continue;
		ref rPuppet = &Characters[sti(arS.chr)];
		CoopNet_Log("test: companion kills 10 crew of " + rPuppet.id);
		CoopNet_SeaBallHit(GetCharacterIndex("CoopTestMate"));
		Ship_ApplyCrewHitpoints(rPuppet, 10.0);
	}
}
int iCoopNetTestMenuTicks = 0;

// COOPNET_TESTMENU=1: ~10 s into the game presses the Esc menu's co-op button twice (its function).
void CoopNet_TestMenu()
{
	if (iCoopNetTestMenuTicks < 0 || CoopNet_GetEnv("COOPNET_TESTMENU") != "1") return;
	if (!bCoopNetWorldInGame) return;
	iCoopNetTestMenuTicks++;
	if (iCoopNetTestMenuTicks == 200 || iCoopNetTestMenuTicks == 300)
	{
		CoopNet_Log("test: co-op button, state " + CoopNet_State());
		CoopNet_MenuInvite();
	}
	if (iCoopNetTestMenuTicks > 300) iCoopNetTestMenuTicks = -1;
}
int iCoopNetTestHeroTicks = 0;

// COOPNET_HEROTEST=1 on a client (a newcomer): once its hero is in place it sails to Bridgetown and
// gets 12345 gold; its hero goes to the host every 15 s here. run_pair.ps1 -TestHero then restarts the
// client without COOPNET_NEWGAME: as a returning player it starts a game by itself, gets its hero
// back and logs (COOPNET_HEROBACK=1) what it has.
void CoopNet_TestHero()
{
	if (iCoopNetTestHeroTicks < 0) return;
	if (CoopNet_State() != COOPNET_CONNECTED || iCoopNetHeroState != 3) return;
	if (CoopNet_GetEnv("COOPNET_HEROBACK") == "1")
	{
		iCoopNetTestHeroTicks++;
		if (iCoopNetTestHeroTicks < 300) return;
		iCoopNetTestHeroTicks = -1;
		CoopNet_Log("test: back with " + pchar.money + " gold in " + pchar.location + ", ship " + RealShips[sti(pchar.Ship.Type)].BaseName);
		return;
	}
	if (CoopNet_GetEnv("COOPNET_HEROTEST") != "1") return;
	iCoopNetTestHeroTicks++;
	if (iCoopNetTestHeroTicks == 100)
	{
		CoopNet_Log("test: hero goes to Bridgetown");
		DoReloadCharacterToLocation("Bridgetown_town", "reload", "reload1");
	}
	if (iCoopNetTestHeroTicks == 300)
	{
		pchar.money = 12345;
		CoopNet_Log("test: hero gets 12345 gold in " + pchar.location);
		iCoopNetTestHeroTicks = -1;
	}
}
int iCoopNetTestRulesTicks = 0;

// COOPNET_RULESTEST=1: the host goes to Bridgetown and, once it keeps the client's hero, saves; the
// client gets 777 gold, plays "boarding" for ~10 s (the host must pause), then dies: the host loads its
// save and the client comes back with the hero that save keeps.
void CoopNet_TestRules()
{
	if (iCoopNetTestRulesTicks < 0 || CoopNet_GetEnv("COOPNET_RULESTEST") != "1") return;
	if (CurrentInterface == INTERFACE_CREATE_CHARACTER) Event("InterfaceBreak"); // its open screen blocks saving and the death screen
	int iState = CoopNet_State();
	if (iState == COOPNET_HOSTING)
	{
		if (!bCoopNetWorldInGame) return;
		iCoopNetTestRulesTicks++;
		if (iCoopNetTestRulesTicks == 100) DoReloadCharacterToLocation("Bridgetown_town", "reload", "reload1");
		if (iCoopNetTestRulesTicks > 600 && GetAttributesNum(&CoopNetGuests) > 0)
		{
			CoopNet_Log("test: host saves, " + GetAttributesNum(&CoopNetGuests) + " heroes kept, in " + pchar.location);
			if (!CheckAttribute(&PlayerProfile, "name")) PlayerProfile.name = "Player"; // the skipped hero selection sets it
			MakeAutoSave(); // no save-place rules, unlike the quick save
			iCoopNetTestRulesTicks = -1;
		}
		return;
	}
	if (iState != COOPNET_CONNECTED || iCoopNetHeroState != 3 || !bCoopNetWorldInGame) return;
	iCoopNetTestRulesTicks++;
	if (iCoopNetTestRulesTicks == 100)
	{
		pchar.money = 777;
		DoReloadCharacterToLocation("Bridgetown_town", "reload", "reload1");
		CoopNet_Log("test: client has 777 gold");
	}
	if (iCoopNetTestRulesTicks == 900)
	{
		CoopNet_Log("test: client boards");
		bAbordageStarted = true;
	}
	if (iCoopNetTestRulesTicks == 1100)
	{
		CoopNet_Log("test: client boarding over");
		bAbordageStarted = false;
	}
	if (iCoopNetTestRulesTicks == 1300)
	{
		pchar.money = 5;
		CoopNet_Log("test: client dies with 5 gold");
		iCoopNetTestRulesTicks = -1;
		GameOver("land");
	}
}

float fCoopNetStatusLogAt = 0.0;

// Every 10 s in a session: where this player and every other one are, for reading coopnet.log afterwards.
void CoopNet_LogStatus()
{
	int iState = CoopNet_State();
	if (iState != COOPNET_CONNECTED && iState != COOPNET_HOSTING) return;
	if (CoopNet_Seconds() < fCoopNetStatusLogAt) return;
	fCoopNetStatusLogAt = CoopNet_Seconds() + 10.0;
	string sLine = "status: me mode " + iCoopNetMode + " in '" + CoopNet_PresencePlace() + "'";
	if (bCoopNetHasMapPos) sLine = sLine + " map " + fCoopNetMapX + " " + fCoopNetMapZ;
	sLine = sLine + ", world in game " + bCoopNetWorldInGame + " synced " + bCoopNetWorldSynced + ", hero state " + iCoopNetHeroState;
	aref arPlayers;
	makearef(arPlayers, CoopNetPlayers);
	int iNum = GetAttributesNum(arPlayers);
	for (int i = 0; i < iNum; i++)
	{
		aref arP = GetAttributeN(arPlayers, i);
		if (sti(arP.id) == CoopNet_MyId()) continue;
		sLine = sLine + "; player " + arP.id + " '" + arP.name + "' mode " + arP.mode + " in '" + arP.place + "'";
		if (sti(arP.hasMap)) sLine = sLine + " map " + arP.x + " " + arP.z;
		if (CheckAttribute(arP, "enc")) sLine = sLine + " avatar " + arP.enc + CoopNet_AvatarOff(arP);
	}
	aref arEncs;
	makearef(arEncs, CoopNetEncs);
	sLine = sLine + "; shared ships " + GetAttributesNum(arEncs);
	CoopNet_Log(sLine);
}

// COOPNET_TESTSKIP=1: ~10 s into the game this player sleeps 8 hours through WaitDate.
void CoopNet_TestSkip()
{
	if (iCoopNetTestSkipTicks < 0 || CoopNet_GetEnv("COOPNET_TESTSKIP") != "1") return;
	if (!CoopNet_SkipVoting()) return;
	iCoopNetTestSkipTicks++;
	if (iCoopNetTestSkipTicks < 200) return;
	iCoopNetTestSkipTicks = -1;
	CoopNet_Log("test: WaitDate 8 hours");
	WaitDate("", 0, 0, 0, 8, 0);
}

int iCoopNetTestWorldTicks = 0;

// COOPNET_TESTWORLD=1: once the world is shared, the host changes store 0 and the owner of colony 0,
// a client changes store 1; ~45 s later both log what they see.
void CoopNet_TestWorld()
{
	if (iCoopNetTestWorldTicks < 0 || CoopNet_GetEnv("COOPNET_TESTWORLD") != "1") return;
	if (!bCoopNetWorldInGame) return;
	int iState = CoopNet_State();
	if (iState == COOPNET_CONNECTED && !bCoopNetWorldSynced) return;
	if (iState == COOPNET_HOSTING && CoopNet_PeerCount() == 0) return;
	iCoopNetTestWorldTicks++;
	string sGood = Goods[GOOD_POWDER].name;
	if (iCoopNetTestWorldTicks == 100 && iState == COOPNET_CONNECTED)
	{
		CoopNet_Log("test: client sets store 1 " + sGood + " to 22222");
		Stores[1].Goods.(sGood).Quantity = 22222;
	}
	if (iCoopNetTestWorldTicks == 600 && iState == COOPNET_HOSTING)
	{
		CoopNet_Log("test: host sets store 0 " + sGood + " to 11111, gives " + Colonies[0].id + " to Spain");
		Stores[0].Goods.(sGood).Quantity = 11111;
		SetCaptureTownByNation(Colonies[0].id, SPAIN);
	}
	if (iCoopNetTestWorldTicks == 900)
	{
		iCoopNetTestWorldTicks = -1;
		CoopNet_Log("test: store 0 " + sGood + " " + Stores[0].Goods.(sGood).Quantity + ", store 1 " + Stores[1].Goods.(sGood).Quantity + ", " + Colonies[0].id + " nation " + Colonies[0].nation + ", store 5 " + CoopNet_WorldStoreState(5));
	}
}

// Logs the shared clock every ~5 s while connected.
void CoopNet_LogClock()
{
	iCoopNetClockLogTicks++;
	if (iCoopNetClockLogTicks < 100) return;
	iCoopNetClockLogTicks = 0;
	float fRate = CoopNet_ClockRate();
	CoopNet_Log("clock " + GetDataYear() + "-" + GetDataMonth() + "-" + GetDataDay() + " " + GetHour() + ":" + GetMinute() + " rate " + fRate);
}

// The tick runs from the engine's "frame" event, timed by real time: posted events stop while the
// game is paused (the Esc menu sets the time scale to 0), and the network, Steam callbacks and invites
// must go on. (A static #event_handler for "frame" is never called; ClearEvents at a game load or a
// teleport may drop the handler, so the time hook puts it back.)
float fCoopNetLastTick = 0.0;

void CoopNet_StartTick()
{
	bCoopNetTicking = true;
	DelEventHandler("frame", "CoopNet_Frame"); // never twice
	SetEventHandler("frame", "CoopNet_Frame", 0);
}

void CoopNet_Frame()
{
	if (!bCoopNetTicking) return;
	if (IsEntity(&worldMap)) CoopNet_MapShipDrive(stf(worldMap.playerShipX), stf(worldMap.playerShipZ)); // every frame: other players' ships on the map
	float fNow = CoopNet_Seconds();
	if (fNow - fCoopNetLastTick < COOPNET_TICK_MS / 1000.0) return;
	fCoopNetLastTick = fNow;
	CoopNet_Tick();
}

void CoopNet_Tick()
{
	CoopNet_UpdateStatus();
	CoopNet_Update();
	CoopNet_PresenceTick();
	CoopNet_WorldTick();
	CoopNet_EncTick();
	CoopNet_SeaTick();
	CoopNet_LandTick();
	CoopNet_HeroTick();
	CoopNet_RulesTick();
	CoopNet_LogClock();
	CoopNet_LogStatus();
	CoopNet_TestNewGame();
	CoopNet_TestSkip();
	CoopNet_TestMap();
	CoopNet_TestWorld();
	CoopNet_TestMenu();
	CoopNet_TestHero();
	CoopNet_TestRules();
	CoopNet_TestLand();
	int iType = CoopNet_Recv();
	while (iType != 0)
	{
		CoopNet_Dispatch(iType);
		iType = CoopNet_Recv();
	}
	if (CoopNet_State() == COOPNET_OFFLINE && !bCoopNetSteam)
	{
		bCoopNetTicking = false;
	}
}

// loadedLocation is an unset ref in the main menu, so location presence is tracked by events.
bool bCoopNetInLocation = false;

#event_handler(EVENT_LOCATION_LOAD, "CoopNet_OnLocationLoad");
#event_handler(EVENT_LOCATION_UNLOAD, "CoopNet_OnLocationUnload");

void CoopNet_OnLocationLoad()
{
	bCoopNetInLocation = true;
}

void CoopNet_OnLocationUnload()
{
	bCoopNetInLocation = false;
	CoopNet_LandForgetScene();
}

// Called at the end of NewGame_continue() and OnLoad() in seadogs.c: the host's world (new or
// loaded) defines the shared time from now on.
void CoopNet_OnGameStarted()
{
	CoopNet_ClockRestart();
	if (bCoopNetTicking) CoopNet_StartTick(); // a game start clears the event handlers
	CoopNet_WorldGameStarted();
	CI_CreateAndSetControls("", "CoopVoteYes", CI_GetKeyCode("VK_F8"), 0, false);
	CI_CreateAndSetControls("", "CoopJoinSea", CI_GetKeyCode("VK_F9"), 0, false);
}

#event_handler("Control Activation", "CoopNet_OnControl");
void CoopNet_OnControl()
{
	string sControl = GetEventData();
	if (sControl == "CoopVoteYes") CoopNet_VoteYes();
	if (sControl == "CoopJoinSea") CoopNet_SeaJoinOffered();
}

// ---- time skips ----------------------------------------------------------------
// AddTimeToCurrent, AddDataToCurrent (calendar.c) and WaitDate (quests.c) call this first.
// In a session the skip becomes a vote; returns true when the caller must not apply it itself.
bool CoopNet_InterceptSkip(int iYear, int iMonth, int iDay, int iHour, int iMinute, string sContinue)
{
	if (!CoopNet_SkipVoting()) return false;
	int iMinutes = (((iYear * 365 + iMonth * 30 + iDay) * 24) + iHour) * 60 + iMinute;
	if (iMinutes <= 0) return false;
	CoopNet_RequestSkip(iMinutes, sContinue);
	return true;
}

// Puts the shared clock on screen now instead of at the next weather update.
void CoopNet_ApplyClockNow()
{
	CoopNet_TimeHook(GetTime(), 0);
	RefreshWeather();
}

// Where this player is and whether it fights: drives the shared clock's pace.
void CoopNet_UpdateStatus()
{
	int iMode = COOPNET_MODE_MENU;
	bool bBattle = false;
	if (IsEntity(&worldMap))
	{
		iMode = COOPNET_MODE_WORLDMAP;
		iCoopNetMode = iMode;
		CoopNet_SetStatus(iMode, false);
		CoopNet_DriveWorldMapClock();
		return;
	}
	else
	{
		if (bSeaActive)
		{
			iMode = COOPNET_MODE_SEA;
			bBattle = bDisableMapEnter; // enemies close enough to block the map
		}
		else
		{
			if (bCoopNetInLocation)
			{
				// a fight on land doesn't stop the shared clock: the others play on
				iMode = COOPNET_MODE_LAND;
			}
		}
	}
	if (bAbordageStarted) bBattle = true;
	iCoopNetMode = iMode;
	CoopNet_SetStatus(iMode, bBattle);
}

// The world map runs its own clock (WorldMap.dll ignores the time written back to worldMap.date),
// so in a session its speed is steered instead: the shared pace plus a correction that closes the
// gap between the map's clock and the shared one within ~10 s.
float fCoopNetMapLag = 0.0; // hours the map's clock is behind the shared one (from the time hook)

void CoopNet_DriveWorldMapClock()
{
	float fNatural = 0.5; // worldmap_init.c default, game hours per second
	if (CheckAttribute(pchar, "wmtimechange")) fNatural = stf(pchar.wmtimechange);
	CoopNet_SetLocalRate(fNatural * 3600.0);

	int iState = CoopNet_State();
	if (iState != COOPNET_CONNECTED && iState != COOPNET_HOSTING)
	{
		worldMap.date.hourPerSec = fNatural;
		return;
	}
	float fSpeed = CoopNet_ClockRate() / 3600.0 + fCoopNetMapLag / 10.0;
	if (fSpeed < 0.0001) fSpeed = 0.0001; // scripts divide by it
	worldMap.date.hourPerSec = fSpeed;
}

// ---- shared clock hooks ----------------------------------------------------
// Called from Whr_TimeUpdate (land/sea) and wdmEvent_UpdateDate (world map), which are compiled
// before this file, hence wrappers instead of #defines at the call sites.

float CoopNet_WeatherTimeHook(float fTime)
{
	return CoopNet_TimeHook(fTime, COOPNET_CLOCK_WEATHER);
}

float CoopNet_WorldMapTimeHook(float fTime)
{
	return CoopNet_TimeHook(fTime, COOPNET_CLOCK_WORLDMAP);
}

// The engine wants to set fTime (hours); replace it with the shared clock and apply the date.
float CoopNet_TimeHook(float fTime, int iSource)
{
	int iState = CoopNet_State();
	bool bInSession = false;
	if (iState == COOPNET_CONNECTED || iState == COOPNET_HOSTING) bInSession = true;
	if (bCoopNetTicking && CoopNet_TickAge() > 1000) CoopNet_StartTick(); // the frame handler was dropped
	if (!bInSession) return fTime;

	float fHours = CoopNet_ClockEngine(GetDataYear(), GetDataMonth(), GetDataDay(), fTime, iSource);
	int iDaysAhead = CoopNet_ClockDaysAhead();
	if (iSource == COOPNET_CLOCK_WORLDMAP) fCoopNetMapLag = iDaysAhead * 24.0 + fHours - fTime;
	if (iDaysAhead != 0)
	{
		int iYear = CoopNet_ClockYear();
		int iMonth = CoopNet_ClockMonth();
		int iDay = CoopNet_ClockDay();
		Environment.date.year = iYear;
		Environment.date.month = iMonth;
		Environment.date.day = iDay;
		worldMap.date.year = iYear;
		worldMap.date.month = iMonth;
		worldMap.date.day = iDay;
		if (iDaysAhead > 0) PostEvent("NextDay", 500); // daily world update, as AddDataToCurrent does
	}

	int iHour = makeint(fHours);
	float fMin = (fHours - iHour) * 60.0;
	int iMin = makeint(fMin);
	Environment.time = fHours;
	Environment.date.hour = iHour; // also keeps Whr_TimeUpdate from rolling the day over again
	Environment.date.min = iMin;
	Environment.date.sec = makeint((fMin - iMin) * 60.0);
	worldMap.date.hour = iHour;
	worldMap.date.min = iMin;
	Weather.Time.time = fHours;
	return fHours;
}

void CoopNet_Dispatch(int iType)
{
	// No 'default:' here: Storm script labels are global and seadogs.c already has one.
	int iFrom = CoopNet_From();
	int iId, iReason, iNum, iPing;
	float fNum;
	string sName, sText;

	if (iType == COOPNET_MSG_PRESENCE)
	{
		CoopNet_OnPresence(iFrom);
		return;
	}
	if (CoopNet_WorldDispatch(iType, iFrom)) return;
	if (CoopNet_EncDispatch(iType, iFrom)) return;
	if (CoopNet_HeroDispatch(iType, iFrom)) return;
	if (CoopNet_RulesDispatch(iType, iFrom)) return;
	if (CoopNet_LandDispatch(iType, iFrom)) return;
	if (iType == COOPNET_MSG_SEA)
	{
		CoopNet_OnSea(iFrom);
		return;
	}
	if (iType == COOPNET_MSG_SEA_HIT)
	{
		CoopNet_OnSeaHit(iFrom);
		return;
	}
	if (iType == COOPNET_MSG_SEA_SINK)
	{
		CoopNet_OnSeaSink(iFrom);
		return;
	}
	if (iType == COOPNET_MSG_SEA_SHOTS)
	{
		CoopNet_OnSeaShots(iFrom);
		return;
	}
	if (iType == COOPNET_MSG_SEA_DAMAGE)
	{
		CoopNet_OnSeaDamage(iFrom);
		return;
	}
	if (iType == COOPNET_MSG_SEA_TAKEN)
	{
		CoopNet_OnSeaTaken(iFrom);
		return;
	}

	if (iType > COOPNET_MSG_TEST_ECHO)
	{
		CoopNet_Log("unknown message type " + iType + " from " + iFrom);
		return;
	}

	switch (iType)
	{
		case COOPNET_EVT_CONNECTED:
			iId = CoopNet_GetInt();
			sName = CoopNet_PeerName(COOPNET_HOST_ID);
			CoopNet_Log("connected to '" + sName + "', my id " + iId);
			CoopNet_Notice("Кооператив: подключено к игре " + sName);
			CoopNet_WorldConnected();
			CoopNet_HeroConnected();
			sText = CoopNet_PeerName(iId);
			CoopNet_Begin(COOPNET_MSG_TEST_HELLO);
			CoopNet_PutString("hello from " + sText);
			CoopNet_PutFloat(1.5);
			CoopNet_PutInt(42);
			CoopNet_Send(COOPNET_HOST_ID, COOPNET_RELIABLE);
		break;

		case COOPNET_EVT_CONNECT_FAILED:
			iReason = CoopNet_GetInt();
			CoopNet_Log("connect failed, reason " + iReason);
			if (bCoopNetAutoHost)
			{
				CoopNet_Log("auto: no friend's game found, hosting");
				CoopNet_StartHosting();
			}
			else
			{
				CoopNet_Notice("Кооператив: не удалось подключиться");
			}
		break;

		case COOPNET_EVT_PEER_JOINED:
			iId = CoopNet_GetInt();
			sName = CoopNet_GetString();
			CoopNet_Log("peer " + iId + " '" + sName + "' joined, " + CoopNet_PeerCount() + " connected");
			CoopNet_Notice("Кооператив: " + sName + " присоединился");
		break;

		case COOPNET_EVT_PEER_LEFT:
			iId = CoopNet_GetInt();
			iReason = CoopNet_GetInt();
			CoopNet_Log("peer " + iId + " left, reason " + iReason);
			sName = "игрок " + iId;
			sText = "p" + iId;
			if (CheckAttribute(&CoopNetPlayers, sText + ".name")) sName = CoopNetPlayers.(sText).name;
			CoopNet_Notice("Кооператив: " + sName + " вышел");
			CoopNet_DropPlayer(iId);
		break;

		case COOPNET_EVT_DISCONNECTED:
			iReason = CoopNet_GetInt();
			CoopNet_Log("disconnected from host, reason " + iReason);
			CoopNet_Notice("Кооператив: связь с хостом потеряна");
		break;

		case COOPNET_EVT_SKIP_VOTE:
			sText = CoopNet_GetString();
			Log_Info(sText);
			if (CoopNet_GetEnv("COOPNET_AUTOVOTE") == "1") CoopNet_VoteYes(); // test instances
		break;

		case COOPNET_EVT_SKIP_RESULT:
			iNum = CoopNet_GetInt();      // approved
			iId = CoopNet_GetInt();       // mine
			sName = CoopNet_GetString();  // continuation
			sText = CoopNet_GetString();
			Log_Info(sText);
			if (iNum) CoopNet_ApplyClockNow();
			if (iId)
			{
				// the quest waiting for this skip goes on either way, so it can't get stuck
				if (sName != "") CompleteQuestName(sName, "");
				QuestsCheck();
			}
		break;

		case COOPNET_EVT_NOTICE:
			sText = CoopNet_GetString();
			Log_Info(sText);
		break;

		case COOPNET_MSG_TEST_HELLO:
			sText = CoopNet_GetString();
			fNum = CoopNet_GetFloat();
			iNum = CoopNet_GetInt();
			CoopNet_Log("HELLO from " + iFrom + ": '" + sText + "' " + fNum + " " + iNum);
			CoopNet_Begin(COOPNET_MSG_TEST_ECHO);
			CoopNet_PutString(sText);
			CoopNet_PutFloat(fNum * 2);
			CoopNet_PutInt(iNum + 1);
			sName = CoopNet_PeerName(COOPNET_HOST_ID);
			CoopNet_PutString(sName);
			CoopNet_Send(iFrom, COOPNET_RELIABLE);
		break;

		case COOPNET_MSG_TEST_ECHO:
			sText = CoopNet_GetString();
			fNum = CoopNet_GetFloat();
			iNum = CoopNet_GetInt();
			sName = CoopNet_GetString();
			iPing = CoopNet_Ping(COOPNET_HOST_ID);
			CoopNet_Log("ECHO from " + sName + ": '" + sText + "' " + fNum + " " + iNum + ", ping " + iPing + " ms");
		break;
	}
}

#include "coop\coop_presence.c"
#include "coop\coop_world.c"
#include "coop\coop_encounters.c"
#include "coop\coop_sea.c"
#include "coop\coop_heroes.c"
#include "coop\coop_rules.c"
#include "coop\coop_land.c"
