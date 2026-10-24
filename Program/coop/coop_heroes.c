// CoopNet heroes: the host keeps every player's hero, so a returning player goes on where it left.
//
// A player is known by a stable id (Steam id; else its name). While in the host's world a client
// sends the host its "hero pack" every minute: the hero (pchar), its companions, officers and
// passengers, their ships (RealShips entries) and where it is. The host keeps the latest pack per
// player in CoopNetGuests, a global object, so it goes into the host's saves with the rest of the
// world.
// On connecting a client asks the host whether it has its hero. A newcomer creates its hero as usual
// (new game) and from then on its pack goes to the host. A returning player's game starts by itself,
// skips the hero creation, and the pack from the host replaces the new hero: characters and ships get
// new indices here (Fellows lists and Ship.Type are remapped), then the hero goes back to the place
// it left (a land location, else the world map where it was).
//
// Trees travel as (depth, name, value) entries in preorder, a few hundred per message; the receiver
// keeps the names of the current path and walks down from the root for every entry.

#define COOPNET_MSG_HERO_QUERY		140		// client -> host: string uid. Answer: HERO_ANSWER
#define COOPNET_MSG_HERO_ANSWER		141		// host -> client: int has (a hero of that uid is kept here)
#define COOPNET_MSG_HERO_REQUEST	142		// client -> host: string uid. Answer: the pack (TREE_*) or HERO_ANSWER 0
#define COOPNET_MSG_TREE_BEGIN		143		// int purpose, string uid
#define COOPNET_MSG_TREE_PART		144		// int n, n x (int depth, string name, string value)
#define COOPNET_MSG_TREE_END		145		// int entries
#define COOPNET_TREE_UPLOAD			1		// purpose: a client's pack for the host to keep
#define COOPNET_TREE_HERO			2		// purpose: the host gives a client its kept hero
#define COOPNET_TREE_PART_SIZE		300		// entries per message
#define COOPNET_HERO_UPLOAD_EVERY	60.0	// seconds
#define COOPNET_HERO_SELECTION		"SelectMainCharacter_Cabine"	// where a new game picks its hero

object CoopNetGuests;		// host: u<uid> = the latest pack of that player (saved with the host's game)
object CoopNetHeroPack;		// client: the pack being sent
object CoopNetTreeIn;		// f<from>: purpose, uid, n, path.d<k>, data (the tree being received)
object CoopNetHeroMap;		// while applying a pack: c<old index> / s<old ship> = the new ones here

// Client state: 0 not known yet, 1 asked, 2 a returning player (hero coming), 3 the hero is in place
int iCoopNetHeroState = 0;
bool bCoopNetHeroReturning = false;		// the host has this player's hero: start a game by itself
bool bCoopNetHeroReloaded = false;		// the host loaded a game: this player's hero comes from it
bool bCoopNetHeroNewGame = false;		// start a new game with hero creation (the host's save doesn't know us)
float fCoopNetHeroUploadAt = 0.0;
int iCoopNetTreeCount = 0;
int iCoopNetTreeInPart = 0;

// ---- identity ---------------------------------------------------------------------------------

string CoopNet_HeroUid()
{
	string sUid = CoopNet_MyUid();
	if (sUid != "") return sUid;
	return "n" + CoopNet_HeroSafe(CoopNet_PeerName(CoopNet_MyId()));
}

// Letters and digits only, so the id can be an attribute name.
string CoopNet_HeroSafe(string sText)
{
	string sAllowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	string sOut = "";
	int iLen = strlen(sText);
	for (int i = 0; i < iLen; i++)
	{
		string sChar = GetSymbol(sText, i);
		if (findsubstr(sAllowed, sChar, 0) >= 0) sOut = sOut + sChar;
	}
	return sOut;
}

// ---- client --------------------------------------------------------------------------------------

// Connected to a host (main menu or in a game): does it keep this player's hero?
void CoopNet_HeroConnected()
{
	iCoopNetHeroState = 1;
	bCoopNetHeroReturning = false;
	CoopNet_Begin(COOPNET_MSG_HERO_QUERY);
	CoopNet_PutString(CoopNet_HeroUid());
	CoopNet_Send(COOPNET_HOST_ID, COOPNET_RELIABLE);
}

void CoopNet_OnHeroAnswer()
{
	int iHas = CoopNet_GetInt();
	if (CoopNet_State() != COOPNET_CONNECTED) return;
	if (!iHas)
	{
		if (bCoopNetHeroReloaded)
		{
			bCoopNetHeroReloaded = false;
			CoopNet_Log("hero: the host's save doesn't know us, a new hero");
			CoopNet_Notice("Кооператив: в этом сохранении хоста вас ещё нет - создайте героя");
			CoopNet_HeroNewGame();
			return;
		}
		CoopNet_Log("hero: the host has no hero of ours");
		if (bCoopNetWorldInGame && bCoopNetWorldSynced) CoopNet_HeroInPlace(); // a new hero: it starts here
		else iCoopNetHeroState = 0;
		return;
	}
	CoopNet_Log("hero: the host keeps our hero");
	bCoopNetHeroReloaded = false;
	bCoopNetHeroReturning = true;
	iCoopNetHeroState = 2;
	if (bCoopNetWorldInGame)
	{
		if (bCoopNetWorldSynced) CoopNet_HeroRequest();
		return;
	}
	CoopNet_Notice("Кооператив: ваш герой сохранён у хоста, продолжаем игру");
}

// Main menu with a hero kept at the host: starts a game by itself (as the New Game button does) and
// skips the hero creation screen; the kept hero replaces the new one once the world has arrived.
void CoopNet_HeroAutoStart()
{
	if (!bCoopNetHeroReturning || bCoopNetWorldInGame) return;
	if (CurrentInterface == INTERFACE_MAINMENU)
	{
		CoopNet_Log("hero: starting a game for the returning hero");
		InterfaceStates.BackEnvironmentIsCreated = true;
		DelEventHandler("backgroundcommand", "ProcCommand");
		interfaceResultCommand = RC_INTERFACE_DO_NEW_GAME;
		EndCancelInterface(true);
	}
}

void CoopNet_HeroSkipCreation()
{
	if (!bCoopNetHeroReturning) return;
	if (CurrentInterface != INTERFACE_CREATE_CHARACTER) return;
	Event("InterfaceBreak");
	bDisableCharacterMenu = false; // locked by the selection cabin (see CoopNet_HeroApply)
	bDisableQuestInterface = false;
}

// From the world sync: the host's world is here. A returning player asks for its hero now; a new
// one starts sending its own.
void CoopNet_HeroWorldArrived()
{
	if (iCoopNetHeroState == 2)
	{
		CoopNet_HeroRequest();
		return;
	}
	if (iCoopNetHeroState == 0) CoopNet_HeroConnected();
}

// The host started or loaded a game: this player's hero is the one that game keeps (or a new one).
void CoopNet_HeroWorldReloaded()
{
	CoopNet_Log("hero: the host loaded a game, our hero comes from it");
	CoopNet_HeroConnected();
	bCoopNetHeroReloaded = true;
}

// A new game with the usual hero creation: from the main menu as its New Game button does, in a game
// through the same interface command.
void CoopNet_HeroNewGame()
{
	if (bCoopNetWorldInGame)
	{
		interfaceResultCommand = RC_INTERFACE_DO_NEW_GAME;
		InterfaceStates.doUnFreeze = false;
		SetEventHandler("frame", "InterfaceDoExit", 0);
		return;
	}
	bCoopNetHeroNewGame = true; // when the main menu is up
}

// A new or loaded game here: the host's kept hero (if any) replaces its hero once the world is in.
void CoopNet_HeroGameStarted()
{
	if (iCoopNetHeroState != 2) iCoopNetHeroState = 0;
}

void CoopNet_HeroRequest()
{
	CoopNet_Log("hero: asking the host for our hero");
	CoopNet_Begin(COOPNET_MSG_HERO_REQUEST);
	CoopNet_PutString(CoopNet_HeroUid());
	CoopNet_Send(COOPNET_HOST_ID, COOPNET_RELIABLE);
}

void CoopNet_HeroInPlace()
{
	iCoopNetHeroState = 3;
	bCoopNetHeroReturning = false;
	fCoopNetHeroUploadAt = CoopNet_Seconds() + 10.0;
}

// Every tick: the returning player's automatic start; a client's hero to the host every minute.
void CoopNet_HeroTick()
{
	int iState = CoopNet_State();
	if (iState != COOPNET_CONNECTED)
	{
		if (iState != COOPNET_CONNECTING)
		{
			iCoopNetHeroState = 0;
			bCoopNetHeroReturning = false;
			bCoopNetHeroReloaded = false;
			bCoopNetHeroNewGame = false;
		}
		return;
	}
	CoopNet_HeroAutoStart();
	if (bCoopNetHeroNewGame && !bCoopNetWorldInGame && CurrentInterface == INTERFACE_MAINMENU)
	{
		bCoopNetHeroNewGame = false;
		CoopNet_Log("hero: starting a new game");
		InterfaceStates.BackEnvironmentIsCreated = true;
		DelEventHandler("backgroundcommand", "ProcCommand");
		interfaceResultCommand = RC_INTERFACE_DO_NEW_GAME;
		EndCancelInterface(true);
	}
	CoopNet_HeroSkipCreation();
	if (iCoopNetHeroState != 3 || !bCoopNetWorldInGame) return;
	if (iCoopNetMode == COOPNET_MODE_MENU || CoopNet_Seconds() < fCoopNetHeroUploadAt) return;
	if (pchar.location == COOPNET_HERO_SELECTION) return; // no hero chosen yet
	float fEvery = COOPNET_HERO_UPLOAD_EVERY;
	if (CoopNet_GetEnv("COOPNET_HEROTEST") == "1") fEvery = 15.0;
	fCoopNetHeroUploadAt = CoopNet_Seconds() + fEvery;
	CoopNet_HeroUpload();
}

// ---- the pack ------------------------------------------------------------------------------------

void CoopNet_HeroUpload()
{
	CoopNet_HeroPack();
	aref arPack;
	makearef(arPack, CoopNetHeroPack);
	int iSent = CoopNet_TreeSend(arPack, COOPNET_TREE_UPLOAD, CoopNet_HeroUid(), COOPNET_HOST_ID);
	CoopNet_Log("hero: sent our hero to the host, " + iSent + " entries, hero menu locked " + bDisableCharacterMenu);
}

void CoopNet_HeroPack()
{
	DeleteAttribute(&CoopNetHeroPack, "");
	CoopNetHeroPack.hero = "";
	aref arDest;
	makearef(arDest, CoopNetHeroPack.hero);
	CopyAttributes(arDest, pchar);
	// the people of the hero: every character index in its Fellows lists
	CoopNetHeroPack.chars = "";
	if (CheckAttribute(pchar, "Fellows"))
	{
		aref arFellows;
		makearef(arFellows, pchar.Fellows);
		CoopNet_HeroCollect(arFellows);
	}
	// their ships
	CoopNetHeroPack.ships = "";
	CoopNet_HeroPackShip(pchar);
	aref arChars;
	makearef(arChars, CoopNetHeroPack.chars);
	int iNum = GetAttributesNum(arChars);
	for (int i = 0; i < iNum; i++)
	{
		aref arChr = GetAttributeN(arChars, i);
		CoopNet_HeroPackShip(arChr);
	}
	// where the hero is
	CoopNetHeroPack.place.mode = iCoopNetMode;
	CoopNetHeroPack.place.location = pchar.location;
	CoopNetHeroPack.place.group = "";
	CoopNetHeroPack.place.locator = "";
	if (CheckAttribute(pchar, "location.group")) CoopNetHeroPack.place.group = pchar.location.group;
	if (CheckAttribute(pchar, "location.locator")) CoopNetHeroPack.place.locator = pchar.location.locator;
	CoopNetHeroPack.place.mapX = fCoopNetMapX;
	CoopNetHeroPack.place.mapZ = fCoopNetMapZ;
	CoopNetHeroPack.place.hasMap = bCoopNetHasMapPos;
}

// Character indices anywhere in a Fellows list (Quantity is a count, not an index).
void CoopNet_HeroCollect(aref arList)
{
	int iNum = GetAttributesNum(arList);
	for (int i = 0; i < iNum; i++)
	{
		aref arItem = GetAttributeN(arList, i);
		if (GetAttributesNum(arItem) > 0) CoopNet_HeroCollect(arItem);
		if (GetAttributeName(arItem) == "Quantity") continue;
		int iChr = CoopNet_HeroIndex(GetAttributeValue(arItem));
		if (iChr <= 0 || iChr >= TOTAL_CHARACTERS || iChr == nMainCharacterIndex) continue;
		string sC = "c" + iChr;
		if (CheckAttribute(&CoopNetHeroPack, "chars." + sC)) continue;
		if (!CheckAttribute(&Characters[iChr], "id")) continue;
		CoopNetHeroPack.chars.(sC) = "";
		aref arDest;
		makearef(arDest, CoopNetHeroPack.chars.(sC));
		CopyAttributes(arDest, &Characters[iChr]);
	}
}

// The number a value holds, -1 when it isn't a plain number.
int CoopNet_HeroIndex(string sValue)
{
	if (sValue == "") return -1;
	int iValue = sti(sValue);
	string sBack = "" + iValue;
	if (sBack != sValue) return -1;
	return iValue;
}

void CoopNet_HeroPackShip(aref arChr)
{
	if (!CheckAttribute(arChr, "Ship.Type")) return;
	int iShip = sti(arChr.Ship.Type);
	if (iShip == SHIP_NOTUSED || iShip < 0) return;
	string sS = "s" + iShip;
	if (CheckAttribute(&CoopNetHeroPack, "ships." + sS)) return;
	CoopNetHeroPack.ships.(sS) = "";
	aref arDest;
	makearef(arDest, CoopNetHeroPack.ships.(sS));
	CopyAttributes(arDest, &RealShips[iShip]);
}

// The kept hero replaces this game's one.
void CoopNet_HeroApply(aref arPack)
{
	DeleteAttribute(&CoopNetHeroMap, "");
	int i, iNum;
	// ships: new RealShips entries
	aref arShips;
	makearef(arShips, arPack.ships);
	iNum = GetAttributesNum(arShips);
	for (i = 0; i < iNum; i++)
	{
		aref arShip = GetAttributeN(arShips, i);
		int iShip = FindFirstEmptyShip();
		if (iShip < 0) break;
		DeleteAttribute(&RealShips[iShip], "");
		CopyAttributes(&RealShips[iShip], arShip);
		RealShips[iShip].index = iShip;
		string sOldShip = GetAttributeName(arShip);
		CoopNetHeroMap.(sOldShip) = iShip;
	}
	// characters: found by id, or new ones
	aref arChars;
	makearef(arChars, arPack.chars);
	iNum = GetAttributesNum(arChars);
	for (i = 0; i < iNum; i++)
	{
		aref arChr = GetAttributeN(arChars, i);
		int iChr = NPC_FindOrCreateCharacter(arChr.id);
		if (iChr < 0) continue;
		DeleteAttribute(&Characters[iChr], "");
		CopyAttributes(&Characters[iChr], arChr);
		Characters[iChr].index = iChr;
		string sOldChr = GetAttributeName(arChr);
		CoopNetHeroMap.(sOldChr) = iChr;
	}
	for (i = 0; i < iNum; i++)
	{
		arChr = GetAttributeN(arChars, i);
		string sC = GetAttributeName(arChr);
		if (CheckAttribute(&CoopNetHeroMap, sC)) CoopNet_HeroRemap(&Characters[sti(CoopNetHeroMap.(sC))]);
	}
	// the hero itself
	int iMain = nMainCharacterIndex;
	aref arHero;
	makearef(arHero, arPack.hero);
	DeleteAttribute(pchar, "");
	CopyAttributes(pchar, arHero);
	pchar.index = iMain;
	CoopNet_HeroRemap(pchar);
	CoopNet_Log("hero: our hero is back, " + iNum + " people, " + GetAttributesNum(arShips) + " ships");
	CoopNet_Notice("Кооператив: ваш герой загружен");
	// the hero selection cabin of the new game locked the hero menu (F2) and the journal till its end
	// (EndSelectMainCharacter), which a returning hero never reaches
	bDisableCharacterMenu = false;
	bDisableQuestInterface = false;
	CoopNet_Log("hero: menus unlocked, interface " + InterfaceStates.Launched + " " + CurrentInterface);
	CoopNet_HeroInPlace();
	CoopNet_HeroGoBack(arPack);
}

// Ship.Type and Fellows of a restored character point at the new indices here.
void CoopNet_HeroRemap(ref rChr)
{
	if (CheckAttribute(rChr, "Ship.Type"))
	{
		string sS = "s" + rChr.Ship.Type;
		if (CheckAttribute(&CoopNetHeroMap, sS)) rChr.Ship.Type = CoopNetHeroMap.(sS);
	}
	if (CheckAttribute(rChr, "Fellows"))
	{
		aref arFellows;
		makearef(arFellows, rChr.Fellows);
		CoopNet_HeroRemapList(arFellows);
	}
}

void CoopNet_HeroRemapList(aref arList)
{
	int iNum = GetAttributesNum(arList);
	for (int i = 0; i < iNum; i++)
	{
		aref arItem = GetAttributeN(arList, i);
		if (GetAttributesNum(arItem) > 0) CoopNet_HeroRemapList(arItem);
		if (GetAttributeName(arItem) == "Quantity") continue;
		int iOld = CoopNet_HeroIndex(GetAttributeValue(arItem));
		if (iOld <= 0) continue;
		string sC = "c" + iOld;
		if (CheckAttribute(&CoopNetHeroMap, sC))
		{
			string sName = GetAttributeName(arItem);
			arList.(sName) = CoopNetHeroMap.(sC);
		}
	}
}

// Back where the hero left: its land location, else the world map where it was.
void CoopNet_HeroGoBack(aref arPack)
{
	int iMode = sti(arPack.place.mode);
	string sLocation = arPack.place.location;
	if (iMode == COOPNET_MODE_LAND && sLocation != "" && sLocation != COOPNET_HERO_SELECTION && FindLocation(sLocation) >= 0)
	{
		string sGroup = arPack.place.group;
		string sLocator = arPack.place.locator;
		if (sGroup == "" || sLocator == "")
		{
			sGroup = "reload";
			sLocator = "reload1";
		}
		CoopNet_Log("hero: back to " + sLocation + " " + sGroup + " " + sLocator);
		DoReloadCharacterToLocation(sLocation, sGroup, sLocator);
		return;
	}
	if (!sti(arPack.place.hasMap)) return; // never was on the map: stays where the new game began
	CoopNet_Log("hero: back to the world map at " + arPack.place.mapX + " " + arPack.place.mapZ);
	worldMap.island = WDM_NONE_ISLAND;
	worldMap.zeroX = arPack.place.mapX;
	worldMap.zeroZ = arPack.place.mapZ;
	pchar.Ship.Pos.x = 0.0;
	pchar.Ship.Pos.z = 0.0;
	if (!CheckAttribute(pchar, "Ship.Ang.y")) pchar.Ship.Ang.y = 0.0;
	Land_MapLoad();
}

// ---- host ----------------------------------------------------------------------------------------

void CoopNet_OnHeroQuery(int iFrom, bool bSend)
{
	string sUid = CoopNet_GetString();
	if (CoopNet_State() != COOPNET_HOSTING) return;
	string sKey = "u" + sUid;
	bool bHas = CheckAttribute(&CoopNetGuests, sKey + ".hero");
	CoopNet_Log("hero: player " + iFrom + " (" + sUid + ") " + "asks, kept: " + bHas);
	if (bSend && bHas)
	{
		aref arPack;
		makearef(arPack, CoopNetGuests.(sKey));
		int iSent = CoopNet_TreeSend(arPack, COOPNET_TREE_HERO, sUid, iFrom);
		CoopNet_Log("hero: sent player " + iFrom + " its hero, " + iSent + " entries");
		return;
	}
	CoopNet_Begin(COOPNET_MSG_HERO_ANSWER);
	CoopNet_PutInt(bHas);
	CoopNet_Send(iFrom, COOPNET_RELIABLE);
}

// A whole tree has arrived from iFrom.
void CoopNet_OnTreeDone(int iFrom, aref arIn)
{
	int iPurpose = sti(arIn.purpose);
	aref arData;
	makearef(arData, arIn.data);
	if (iPurpose == COOPNET_TREE_UPLOAD && CoopNet_State() == COOPNET_HOSTING)
	{
		string sKey = "u" + arIn.uid;
		DeleteAttribute(&CoopNetGuests, sKey);
		CoopNetGuests.(sKey) = "";
		aref arKeep;
		makearef(arKeep, CoopNetGuests.(sKey));
		CopyAttributes(arKeep, arData);
		CoopNet_Log("hero: kept the hero of player " + iFrom + " (" + arIn.uid + "), " + arIn.n + " entries");
		return;
	}
	if (iPurpose == COOPNET_TREE_HERO && CoopNet_State() == COOPNET_CONNECTED && iFrom == COOPNET_HOST_ID)
	{
		CoopNet_HeroApply(arData);
	}
}

// ---- trees over the network ------------------------------------------------------------------------

// Sends a tree in preorder; returns the number of entries.
int CoopNet_TreeSend(aref arRoot, int iPurpose, string sUid, int iTo)
{
	CoopNet_Begin(COOPNET_MSG_TREE_BEGIN);
	CoopNet_PutInt(iPurpose);
	CoopNet_PutString(sUid);
	CoopNet_Send(iTo, COOPNET_RELIABLE);
	iCoopNetTreeCount = 0;
	iCoopNetTreeInPart = 0;
	CoopNet_TreeSendNode(arRoot, 0, iTo);
	if (iCoopNetTreeInPart > 0) CoopNet_TreeFlush(iTo);
	CoopNet_Begin(COOPNET_MSG_TREE_END);
	CoopNet_PutInt(iCoopNetTreeCount);
	CoopNet_Send(iTo, COOPNET_RELIABLE);
	return iCoopNetTreeCount;
}

// A part message is built in CoopNetTreeOut and sent when full: the entry count leads it.
object CoopNetTreeOut;

void CoopNet_TreeSendNode(aref arNode, int iDepth, int iTo)
{
	int iNum = GetAttributesNum(arNode);
	for (int i = 0; i < iNum; i++)
	{
		aref arItem = GetAttributeN(arNode, i);
		string sE = "e" + iCoopNetTreeInPart;
		CoopNetTreeOut.(sE).d = iDepth;
		CoopNetTreeOut.(sE).n = GetAttributeName(arItem);
		CoopNetTreeOut.(sE).v = GetAttributeValue(arItem);
		iCoopNetTreeInPart++;
		iCoopNetTreeCount++;
		if (iCoopNetTreeInPart >= COOPNET_TREE_PART_SIZE) CoopNet_TreeFlush(iTo);
		if (GetAttributesNum(arItem) > 0) CoopNet_TreeSendNode(arItem, iDepth + 1, iTo);
	}
}

void CoopNet_TreeFlush(int iTo)
{
	CoopNet_Begin(COOPNET_MSG_TREE_PART);
	CoopNet_PutInt(iCoopNetTreeInPart);
	for (int i = 0; i < iCoopNetTreeInPart; i++)
	{
		string sE = "e" + i;
		CoopNet_PutInt(sti(CoopNetTreeOut.(sE).d));
		CoopNet_PutString(CoopNetTreeOut.(sE).n);
		CoopNet_PutString(CoopNetTreeOut.(sE).v);
	}
	CoopNet_Send(iTo, COOPNET_RELIABLE);
	DeleteAttribute(&CoopNetTreeOut, "");
	iCoopNetTreeInPart = 0;
}

void CoopNet_OnTreeBegin(int iFrom)
{
	int iPurpose = CoopNet_GetInt();
	string sUid = CoopNet_GetString();
	string sF = "f" + iFrom;
	DeleteAttribute(&CoopNetTreeIn, sF);
	CoopNetTreeIn.(sF).purpose = iPurpose;
	CoopNetTreeIn.(sF).uid = sUid;
	CoopNetTreeIn.(sF).n = 0;
	CoopNetTreeIn.(sF).data = "";
	CoopNetTreeIn.(sF).path = "";
}

void CoopNet_OnTreePart(int iFrom)
{
	int iNum = CoopNet_GetInt();
	string sF = "f" + iFrom;
	bool bOpen = CheckAttribute(&CoopNetTreeIn, sF + ".data");
	aref arIn, arPath;
	if (bOpen)
	{
		makearef(arIn, CoopNetTreeIn.(sF));
		makearef(arPath, arIn.path);
	}
	for (int i = 0; i < iNum; i++)
	{
		int iDepth = CoopNet_GetInt();
		string sName = CoopNet_GetString();
		string sValue = CoopNet_GetString();
		if (!bOpen) continue;
		aref arNode;
		makearef(arNode, arIn.data);
		for (int k = 0; k < iDepth; k++)
		{
			string sD = "d" + k;
			string sStep = arPath.(sD);
			makearef(arNode, arNode.(sStep));
		}
		arNode.(sName) = sValue;
		string sMine = "d" + iDepth;
		arPath.(sMine) = sName;
	}
	if (bOpen) arIn.n = sti(arIn.n) + iNum;
}

void CoopNet_OnTreeEnd(int iFrom)
{
	int iCount = CoopNet_GetInt();
	string sF = "f" + iFrom;
	if (!CheckAttribute(&CoopNetTreeIn, sF + ".data")) return;
	aref arIn;
	makearef(arIn, CoopNetTreeIn.(sF));
	if (sti(arIn.n) != iCount) CoopNet_Log("hero: tree from " + iFrom + " has " + arIn.n + " of " + iCount + " entries");
	else CoopNet_OnTreeDone(iFrom, arIn);
	DeleteAttribute(&CoopNetTreeIn, sF);
}

// Returns true when the message was a hero or tree message.
bool CoopNet_HeroDispatch(int iType, int iFrom)
{
	if (iType < COOPNET_MSG_HERO_QUERY || iType > COOPNET_MSG_TREE_END) return false;
	if (iType == COOPNET_MSG_HERO_QUERY) CoopNet_OnHeroQuery(iFrom, false);
	if (iType == COOPNET_MSG_HERO_REQUEST) CoopNet_OnHeroQuery(iFrom, true);
	if (iType == COOPNET_MSG_HERO_ANSWER) CoopNet_OnHeroAnswer();
	if (iType == COOPNET_MSG_TREE_BEGIN) CoopNet_OnTreeBegin(iFrom);
	if (iType == COOPNET_MSG_TREE_PART) CoopNet_OnTreePart(iFrom);
	if (iType == COOPNET_MSG_TREE_END) CoopNet_OnTreeEnd(iFrom);
	return true;
}
