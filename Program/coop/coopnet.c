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

#library "COOPNET"

#event_handler("CoopNet_Tick", "CoopNet_Tick");

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

// coop.ini next to Maelstrom.exe:
//   [coop]
//   mode=host | join | off
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
	if (sMode != "host" && sMode != "join") return;

	int iPort = COOPNET_DEFAULT_PORT;
	string sValue = CoopNet_Config("port");
	if (sValue != "") iPort = sti(sValue);
	int iMax = 4;
	sValue = CoopNet_Config("maxplayers");
	if (sValue != "") iMax = sti(sValue);
	string sName = CoopNet_Config("name");
	string sAddress = CoopNet_Config("address");
	if (!bCoopNetSteam && sAddress == "") sAddress = "127.0.0.1";

	bool bOk;
	if (sMode == "host") bOk = CoopNet_Host(iPort, iMax, sName);
	else bOk = CoopNet_Connect(sAddress, iPort, sName);
	if (bOk) CoopNet_StartTick();
	else CoopNet_Log(sMode + " failed: " + CoopNet_LastError());
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

// COOPNET_TOMAP=1: goes to the world map once in the game; the host then jumps 40 map units every
// 5 s (through playerShipUpdate) so the client's avatar of it has something to follow.
void CoopNet_TestMap()
{
	if (iCoopNetTestMapTicks < 0 || CoopNet_GetEnv("COOPNET_TOMAP") != "1") return;
	if (!CoopNet_SkipVoting()) return;
	iCoopNetTestMapTicks++;
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
	if (iCoopNetTestMapTicks - (iCoopNetTestMapTicks / 100) * 100 != 0 || iCoopNetTestMapTicks > 1000) return;
	float fX = stf(worldMap.playerShipX) + 40.0;
	worldMap.playerShipX = fX;
	worldMap.playerShipUpdate = "";
	CoopNet_Log("test: host jumps to x " + fX + ", map says " + worldMap.playerShipX);
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

// Each (re)start bumps the generation; ticks of an older chain end themselves, so a watchdog
// restart while the old chain is still alive can't multiply the ticks.
int iCoopNetTickGen = 0;

void CoopNet_StartTick()
{
	if (bCoopNetTicking) return;
	bCoopNetTicking = true;
	iCoopNetTickGen++;
	PostEvent("CoopNet_Tick", COOPNET_TICK_MS, "l", iCoopNetTickGen);
}

void CoopNet_Tick()
{
	int iGen = GetEventData();
	if (iGen != iCoopNetTickGen) return;
	CoopNet_UpdateStatus();
	CoopNet_Update();
	CoopNet_PresenceTick();
	CoopNet_WorldTick();
	CoopNet_EncTick();
	CoopNet_SeaTick();
	CoopNet_LogClock();
	CoopNet_TestNewGame();
	CoopNet_TestSkip();
	CoopNet_TestMap();
	CoopNet_TestWorld();
	int iType = CoopNet_Recv();
	while (iType != 0)
	{
		CoopNet_Dispatch(iType);
		iType = CoopNet_Recv();
	}
	if (CoopNet_State() == COOPNET_OFFLINE && !bCoopNetSteam)
	{
		bCoopNetTicking = false;
		return;
	}
	PostEvent("CoopNet_Tick", COOPNET_TICK_MS, "l", iGen);
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
}

// Called at the end of NewGame_continue() and OnLoad() in seadogs.c: the host's world (new or
// loaded) defines the shared time from now on.
void CoopNet_OnGameStarted()
{
	CoopNet_ClockRestart();
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
				iMode = COOPNET_MODE_LAND;
				bBattle = LAi_group_IsActivePlayerAlarm();
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
	bool bNeedTick = bInSession;
	if (bCoopNetSteam) bNeedTick = true;
	if (bNeedTick && CoopNet_TickAge() > 1000)
	{
		// ClearEvents() (game load, teleport) dropped the posted tick
		bCoopNetTicking = false;
		CoopNet_StartTick();
	}
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
	if (iType == COOPNET_MSG_SEA)
	{
		CoopNet_OnSea(iFrom);
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
			CoopNet_WorldConnected();
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
		break;

		case COOPNET_EVT_PEER_JOINED:
			iId = CoopNet_GetInt();
			sName = CoopNet_GetString();
			CoopNet_Log("peer " + iId + " '" + sName + "' joined, " + CoopNet_PeerCount() + " connected");
		break;

		case COOPNET_EVT_PEER_LEFT:
			iId = CoopNet_GetInt();
			iReason = CoopNet_GetInt();
			CoopNet_Log("peer " + iId + " left, reason " + iReason);
			CoopNet_DropPlayer(iId);
		break;

		case COOPNET_EVT_DISCONNECTED:
			iReason = CoopNet_GetInt();
			CoopNet_Log("disconnected from host, reason " + iReason);
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
