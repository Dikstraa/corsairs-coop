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

#library "COOPNET"

#event_handler("CoopNet_Tick", "CoopNet_Tick");

bool bCoopNetTicking = false;

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
	if (CoopNet_GetEnv("COOPNET_NEWGAME") == "1") PostEvent("CoopNet_TestNewGame", 1000);
}

// ---- test helpers ------------------------------------------------------------

int iCoopNetTestMenuSeen = 0;

#event_handler("CoopNet_TestNewGame", "CoopNet_TestNewGame");
// Presses "New game" once the main menu has been up for a couple of seconds.
void CoopNet_TestNewGame()
{
	if (CurrentInterface != INTERFACE_MAINMENU || iCoopNetTestMenuSeen < 3)
	{
		if (CurrentInterface == INTERFACE_MAINMENU) iCoopNetTestMenuSeen++;
		PostEvent("CoopNet_TestNewGame", 1000);
		return;
	}
	CoopNet_Log("test: starting a new game");
	// what IDoExit in INTERFACE/mainmenu.c does
	InterfaceStates.BackEnvironmentIsCreated = true;
	DelEventHandler("backgroundcommand", "ProcCommand");
	interfaceResultCommand = RC_INTERFACE_DO_NEW_GAME;
	EndCancelInterface(true);
}

int iCoopNetClockLogTicks = 0;

// Logs the shared clock every ~5 s while connected.
void CoopNet_LogClock()
{
	iCoopNetClockLogTicks++;
	if (iCoopNetClockLogTicks < 100) return;
	iCoopNetClockLogTicks = 0;
	float fRate = CoopNet_ClockRate();
	CoopNet_Log("clock " + GetDataYear() + "-" + GetDataMonth() + "-" + GetDataDay() + " " + GetHour() + ":" + GetMinute() + " rate " + fRate);
}

void CoopNet_StartTick()
{
	if (bCoopNetTicking) return;
	bCoopNetTicking = true;
	PostEvent("CoopNet_Tick", COOPNET_TICK_MS);
}

void CoopNet_Tick()
{
	CoopNet_UpdateStatus();
	CoopNet_Update();
	CoopNet_LogClock();
	int iType = CoopNet_Recv();
	while (iType != 0)
	{
		CoopNet_Dispatch(iType);
		iType = CoopNet_Recv();
	}
	if (CoopNet_State() == COOPNET_OFFLINE)
	{
		bCoopNetTicking = false;
		return;
	}
	PostEvent("CoopNet_Tick", COOPNET_TICK_MS);
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
}

// Where this player is and whether it fights: drives the shared clock's pace.
void CoopNet_UpdateStatus()
{
	int iMode = COOPNET_MODE_MENU;
	bool bBattle = false;
	if (IsEntity(&worldMap))
	{
		iMode = COOPNET_MODE_WORLDMAP;
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
	CoopNet_SetStatus(iMode, bBattle);
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
	if (iState != COOPNET_CONNECTED && iState != COOPNET_HOSTING) return fTime;
	if (CoopNet_TickAge() > 1000)
	{
		// ClearEvents() (game load, teleport) dropped the posted tick
		bCoopNetTicking = false;
		CoopNet_StartTick();
	}

	float fHours = CoopNet_ClockEngine(GetDataYear(), GetDataMonth(), GetDataDay(), fTime, iSource);
	int iDaysAhead = CoopNet_ClockDaysAhead();
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
		break;

		case COOPNET_EVT_DISCONNECTED:
			iReason = CoopNet_GetInt();
			CoopNet_Log("disconnected from host, reason " + iReason);
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
