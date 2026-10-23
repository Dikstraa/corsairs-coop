// CoopNet world: the parts of the world everybody shares, owned by the host.
//
// Shared: store goods (quantity, price drift, trade type), colony owners and the relations between
// nations. Each player keeps its own hero, quests and personal places (the Caiman colony and its
// warehouse). Every shared entry has a key (s<store>, c<colony>, r) and a text snapshot of its
// state. Each player keeps the snapshot it last sent or received per key and scans a few keys per
// tick: a key whose state no longer matches its snapshot was changed here (trade, capture, the
// host's daily world update) and goes out - from a client to the host, from the host to everybody.
// The host applies what a client sends and passes it on, so the last change wins everywhere.
//
// A client joining takes the host's world as is: on every game start (new game or load) it asks
// the host for all entries and only starts sending its own changes after that. Clients don't run
// the daily world update themselves (UpdateStore, ChangeImport skip it), it comes from the host.

#define COOPNET_MSG_WORLD_STORE		111		// int store, int goods, then per good: string name, quantity, price drift, trade type, not used
#define COOPNET_MSG_WORLD_COLONY	112		// int colony, string nation
#define COOPNET_MSG_WORLD_RELATIONS	113		// int count, then count ints
#define COOPNET_MSG_WORLD_REQUEST	114		// client -> host: send me the whole world
#define COOPNET_MSG_WORLD_DONE		115		// host -> client: that was all of it
#define COOPNET_WORLD_SCAN			4		// keys checked per tick

object CoopNetWorld;	// key -> snapshot last sent or received
bool bCoopNetWorldInGame = false;	// a game (new or loaded) is running here
bool bCoopNetWorldSynced = false;	// client: the host's world has arrived
int iCoopNetWorldScan = 0;

// Called from CoopNet_OnGameStarted: a new world here.
void CoopNet_WorldGameStarted()
{
	bCoopNetWorldInGame = true;
	bCoopNetWorldSynced = false;
	DeleteAttribute(&CoopNetWorld, "");
	iCoopNetWorldScan = 0;
	CoopNet_EncWorldStarted();
	// host: every key differs from the empty snapshots, so the new world goes out to everybody
	if (CoopNet_State() == COOPNET_CONNECTED) CoopNet_WorldRequest();
}

// Connected while a game is already running here (a Steam invite accepted in the game).
void CoopNet_WorldConnected()
{
	bCoopNetWorldSynced = false;
	DeleteAttribute(&CoopNetWorld, "");
	if (!bCoopNetWorldInGame) return;
	CoopNet_EncReset();
	CoopNet_WorldRequest();
}

void CoopNet_WorldRequest()
{
	CoopNet_Log("world: asking the host for its world");
	CoopNet_Begin(COOPNET_MSG_WORLD_REQUEST);
	CoopNet_Send(COOPNET_HOST_ID, COOPNET_RELIABLE);
}

// For UpdateStore and ChangeImport (compiled before this file): true on a client, whose world
// changes come from the host.
bool CoopNet_WorldFollower()
{
	return CoopNet_State() == COOPNET_CONNECTED;
}

void CoopNet_WorldTick()
{
	int iState = CoopNet_State();
	if (!bCoopNetWorldInGame) return;
	if (iState == COOPNET_OFFLINE || iState == COOPNET_CONNECTING)
	{
		// left the session: the next one starts from scratch
		bCoopNetWorldSynced = false;
		DeleteAttribute(&CoopNetWorld, "");
		return;
	}
	if (iState == COOPNET_CONNECTED && !bCoopNetWorldSynced) return;
	int iTo = COOPNET_HOST_ID;
	if (iState == COOPNET_HOSTING)
	{
		if (CoopNet_PeerCount() == 0) return;
		iTo = COOPNET_ALL;
	}
	int iKeys = CoopNet_WorldKeyCount();
	for (int i = 0; i < COOPNET_WORLD_SCAN; i++)
	{
		if (iCoopNetWorldScan >= iKeys) iCoopNetWorldScan = 0;
		CoopNet_WorldCheckKey(iCoopNetWorldScan, iTo);
		iCoopNetWorldScan++;
	}
}

// Keys by number: stores, then colonies, then the relations.
int CoopNet_WorldKeyCount()
{
	return STORE_QUANTITY + MAX_COLONIES + 1;
}

void CoopNet_WorldCheckKey(int iKey, int iTo)
{
	string sKey, sNow;
	if (iKey < STORE_QUANTITY)
	{
		if (!CoopNet_WorldSharedStore(iKey)) return;
		sKey = "s" + iKey;
		sNow = CoopNet_WorldStoreState(iKey);
	}
	else
	{
		if (iKey < STORE_QUANTITY + MAX_COLONIES)
		{
			iKey = iKey - STORE_QUANTITY;
			if (!CoopNet_WorldSharedColony(iKey)) return;
			sKey = "c" + iKey;
			sNow = Colonies[iKey].nation;
		}
		else
		{
			sKey = "r";
			sNow = CoopNet_WorldRelationsState();
		}
	}
	if (CheckAttribute(&CoopNetWorld, sKey))
	{
		if (CoopNetWorld.(sKey) == sNow) return;
	}
	CoopNetWorld.(sKey) = sNow;
	CoopNet_WorldSendKey(sKey, iTo);
}

void CoopNet_WorldSendKey(string sKey, int iTo)
{
	string sKind = strcut(sKey, 0, 0);
	if (sKind == "r")
	{
		CoopNet_WorldSendRelations(iTo);
		return;
	}
	int iIndex = sti(strcut(sKey, 1, strlen(sKey) - 1));
	if (sKind == "s") CoopNet_WorldSendStore(iIndex, iTo);
	if (sKind == "c")
	{
		CoopNet_Begin(COOPNET_MSG_WORLD_COLONY);
		CoopNet_PutInt(iIndex);
		CoopNet_PutString(Colonies[iIndex].nation);
		CoopNet_Send(iTo, COOPNET_RELIABLE);
	}
}

// Host: the whole world to a client that asked for it. The snapshots stay as they are: changes
// not sent yet still go to everybody with the next scan.
void CoopNet_WorldSendAll(int iTo)
{
	int iSent = 1; // the relations
	int i;
	for (i = 0; i < STORE_QUANTITY; i++)
	{
		if (!CoopNet_WorldSharedStore(i)) continue;
		CoopNet_WorldSendStore(i, iTo);
		iSent++;
	}
	for (i = 0; i < MAX_COLONIES; i++)
	{
		if (!CoopNet_WorldSharedColony(i)) continue;
		CoopNet_WorldSendKey("c" + i, iTo);
		iSent++;
	}
	CoopNet_WorldSendRelations(iTo);
	CoopNet_EncSendAll(iTo);
	CoopNet_Begin(COOPNET_MSG_WORLD_DONE);
	CoopNet_Send(iTo, COOPNET_RELIABLE);
	CoopNet_Log("world: sent " + iSent + " entries to player " + iTo);
}

// ---- what is shared ---------------------------------------------------------------

// The warehouse, the ship-to-ship store and the hero's own colony belong to each player.
bool CoopNet_WorldSharedStore(int iStore)
{
	if (iStore >= STORE_HOUSE) return false;
	if (CheckAttribute(&Stores[iStore], "StoreHouse")) return false;
	if (!CheckAttribute(&Stores[iStore], "Colony")) return false;
	if (Stores[iStore].Colony == "Caiman") return false;
	return CheckAttribute(&Stores[iStore], "Goods");
}

bool CoopNet_WorldSharedColony(int iColony)
{
	if (Colonies[iColony].id == "" || Colonies[iColony].id == "Caiman") return false;
	return CheckAttribute(&Colonies[iColony], "nation");
}

// ---- stores --------------------------------------------------------------------------

string CoopNet_WorldGoodField(aref arGood, string sField)
{
	if (CheckAttribute(arGood, sField)) return arGood.(sField);
	return "-";
}

string CoopNet_WorldStoreState(int iStore)
{
	aref arGoods;
	makearef(arGoods, Stores[iStore].Goods);
	string sState = "";
	int iNum = GetAttributesNum(arGoods);
	for (int i = 0; i < iNum; i++)
	{
		aref arGood = GetAttributeN(arGoods, i);
		sState = sState + GetAttributeName(arGood) + ":" + CoopNet_WorldGoodField(arGood, "Quantity") + "," + CoopNet_WorldGoodField(arGood, "RndPriceModify") + "," + CoopNet_WorldGoodField(arGood, "TradeType") + "," + CoopNet_WorldGoodField(arGood, "NotUsed") + ";";
	}
	return sState;
}

void CoopNet_WorldSendStore(int iStore, int iTo)
{
	aref arGoods;
	makearef(arGoods, Stores[iStore].Goods);
	int iNum = GetAttributesNum(arGoods);
	CoopNet_Begin(COOPNET_MSG_WORLD_STORE);
	CoopNet_PutInt(iStore);
	CoopNet_PutInt(iNum);
	for (int i = 0; i < iNum; i++)
	{
		aref arGood = GetAttributeN(arGoods, i);
		CoopNet_PutString(GetAttributeName(arGood));
		CoopNet_PutString(CoopNet_WorldGoodField(arGood, "Quantity"));
		CoopNet_PutString(CoopNet_WorldGoodField(arGood, "RndPriceModify"));
		CoopNet_PutString(CoopNet_WorldGoodField(arGood, "TradeType"));
		CoopNet_PutString(CoopNet_WorldGoodField(arGood, "NotUsed"));
	}
	CoopNet_Send(iTo, COOPNET_RELIABLE);
}

void CoopNet_WorldSetGoodField(aref arGood, string sField, string sValue)
{
	if (sValue != "-") arGood.(sField) = sValue;
}

void CoopNet_OnWorldStore()
{
	int iStore = CoopNet_GetInt();
	int iNum = CoopNet_GetInt();
	bool bOk = false;
	if (iStore >= 0 && iStore < STORE_QUANTITY) bOk = CoopNet_WorldSharedStore(iStore);
	aref arGoods;
	if (bOk) makearef(arGoods, Stores[iStore].Goods);
	for (int i = 0; i < iNum; i++)
	{
		string sGood = CoopNet_GetString();
		string sQuantity = CoopNet_GetString();
		string sPrice = CoopNet_GetString();
		string sType = CoopNet_GetString();
		string sNotUsed = CoopNet_GetString();
		if (!bOk) continue;
		if (!CheckAttribute(arGoods, sGood)) continue;
		aref arGood;
		makearef(arGood, arGoods.(sGood));
		CoopNet_WorldSetGoodField(arGood, "Quantity", sQuantity);
		CoopNet_WorldSetGoodField(arGood, "RndPriceModify", sPrice);
		CoopNet_WorldSetGoodField(arGood, "TradeType", sType);
		CoopNet_WorldSetGoodField(arGood, "NotUsed", sNotUsed);
	}
	if (!bOk) return;
	string sKey = "s" + iStore;
	CoopNetWorld.(sKey) = CoopNet_WorldStoreState(iStore);
	CoopNet_WorldRelay(sKey);
}

// ---- colonies ----------------------------------------------------------------------

void CoopNet_OnWorldColony()
{
	int iColony = CoopNet_GetInt();
	string sNation = CoopNet_GetString();
	if (iColony < 0 || iColony >= MAX_COLONIES) return;
	if (!CoopNet_WorldSharedColony(iColony)) return;
	if (Colonies[iColony].nation != sNation)
	{
		CoopNet_Log("world: " + Colonies[iColony].id + " now belongs to nation " + sNation);
		if (sNation == "none")
		{
			Colonies[iColony].nation = sNation;
		}
		else
		{
			// the capture as the game does it (mayor, garrison, map flag), but deposits and the
			// store stay: the store comes with the world, deposits are this player's own
			pchar.questTemp.DontNullDeposit = true;
			SetCaptureTownByNation(Colonies[iColony].id, sti(sNation));
		}
	}
	string sKey = "c" + iColony;
	CoopNetWorld.(sKey) = Colonies[iColony].nation;
	CoopNet_WorldRelay(sKey);
}

// ---- relations between nations ------------------------------------------------------

string CoopNet_WorldRelationsState()
{
	string sState = "";
	for (int i = 0; i < MAX_NATIONS_SQR; i++)
	{
		sState = sState + NationsRelations[i] + ",";
	}
	return sState;
}

void CoopNet_WorldSendRelations(int iTo)
{
	CoopNet_Begin(COOPNET_MSG_WORLD_RELATIONS);
	CoopNet_PutInt(MAX_NATIONS_SQR);
	for (int i = 0; i < MAX_NATIONS_SQR; i++)
	{
		CoopNet_PutInt(NationsRelations[i]);
	}
	CoopNet_Send(iTo, COOPNET_RELIABLE);
}

void CoopNet_OnWorldRelations()
{
	int iNum = CoopNet_GetInt();
	for (int i = 0; i < iNum; i++)
	{
		int iRelation = CoopNet_GetInt();
		if (i < MAX_NATIONS_SQR) NationsRelations[i] = iRelation;
	}
	CoopNetWorld.r = CoopNet_WorldRelationsState();
	CoopNet_WorldRelay("r");
}

// ---- traffic --------------------------------------------------------------------------

// Host: a client's change goes on to everybody (the sender gets it back, which changes nothing).
void CoopNet_WorldRelay(string sKey)
{
	if (CoopNet_State() == COOPNET_HOSTING) CoopNet_WorldSendKey(sKey, COOPNET_ALL);
}

// Returns true when the message was a world message.
bool CoopNet_WorldDispatch(int iType, int iFrom)
{
	if (iType < COOPNET_MSG_WORLD_STORE || iType > COOPNET_MSG_WORLD_DONE) return false;
	if (!bCoopNetWorldInGame) return true; // no world here yet (main menu): it is asked for at game start
	int iState = CoopNet_State();
	if (iType == COOPNET_MSG_WORLD_REQUEST)
	{
		if (iState == COOPNET_HOSTING) CoopNet_WorldSendAll(iFrom);
		return true;
	}
	if (iType == COOPNET_MSG_WORLD_DONE)
	{
		bCoopNetWorldSynced = true;
		CoopNet_Log("world: the host's world has arrived");
		Log_Info("CoopNet: the host's world has arrived");
		return true;
	}
	if (iState == COOPNET_CONNECTED && iFrom != COOPNET_HOST_ID) return true;
	if (iType == COOPNET_MSG_WORLD_STORE) CoopNet_OnWorldStore();
	if (iType == COOPNET_MSG_WORLD_COLONY) CoopNet_OnWorldColony();
	if (iType == COOPNET_MSG_WORLD_RELATIONS) CoopNet_OnWorldRelations();
	return true;
}
