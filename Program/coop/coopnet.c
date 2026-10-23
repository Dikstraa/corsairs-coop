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
}

void CoopNet_StartTick()
{
	if (bCoopNetTicking) return;
	bCoopNetTicking = true;
	PostEvent("CoopNet_Tick", COOPNET_TICK_MS);
}

void CoopNet_Tick()
{
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
