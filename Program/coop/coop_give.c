// CoopNet give: money and items from one player to another.
//
// On land, next to another player (its puppet), F4 opens the game's chest window over an empty
// box. Whatever this player puts in it (money goes as gold) is sent to that player when the window
// closes, and its game adds it to the hero. When that player is gone by then, it all comes back.

#define COOPNET_MSG_GIVE		165		// int to, int from, string fromName, int money, int n, n x (string item, int qty)
#define COOPNET_GIVE_NEAR		4.0		// metres to the other player at most

object CoopNetGiveBox;		// the box the chest window shows: items.<id>, money
object CoopNetGiveScratch;	// a message's items while they are passed on
bool bCoopNetGiveOpen = false;
int iCoopNetGiveTo = -1;
string sCoopNetGiveToName = "";

// F4: the box for the nearest other player.
void CoopNet_GiveOffered()
{
	int iState = CoopNet_State();
	if (iState != COOPNET_CONNECTED && iState != COOPNET_HOSTING) return;
	if (bCoopNetGiveOpen || dialogRun || sti(InterfaceStates.Launched))
	{
		CoopNet_Log("give: not now (open " + bCoopNetGiveOpen + ", dialog " + dialogRun + ", interface " + InterfaceStates.Launched + " " + CurrentInterface + ")");
		return;
	}
	string sHere = CoopNet_LandHere();
	if (sHere == "")
	{
		CoopNet_Notice("Кооператив: передавать вещи можно на суше, рядом с другим игроком");
		return;
	}
	float fX = 0.0;
	float fY = 0.0;
	float fZ = 0.0;
	if (SendMessage(pchar, "leee", MSG_CHARACTER_GETPOS, &fX, &fY, &fZ) == 0) return;
	int iBest = -1;
	string sBestName = "";
	float fBest = COOPNET_GIVE_NEAR;
	aref arAll;
	makearef(arAll, CoopNetLand);
	int iNum = GetAttributesNum(arAll);
	for (int i = 0; i < iNum; i++)
	{
		aref arP = GetAttributeN(arAll, i);
		if (arP.loc != sHere || !CheckAttribute(arP, "chr")) continue;
		ref rChr = &Characters[sti(arP.chr)];
		float fPX = 0.0;
		float fPY = 0.0;
		float fPZ = 0.0;
		if (SendMessage(rChr, "leee", MSG_CHARACTER_GETPOS, &fPX, &fPY, &fPZ) == 0) continue;
		float fDist = sqrt((fPX - fX) * (fPX - fX) + (fPZ - fZ) * (fPZ - fZ));
		if (fDist <= fBest)
		{
			fBest = fDist;
			iBest = sti(arP.id);
			sBestName = arP.name;
		}
	}
	if (iBest < 0)
	{
		CoopNet_Notice("Кооператив: подойдите к другому игроку ближе и нажмите F4, чтобы передать вещи");
		return;
	}
	DeleteAttribute(&CoopNetGiveBox, "");
	CoopNetGiveBox.money = 0;
	CoopNetGiveBox.items = "";
	LaunchItemsBox(&CoopNetGiveBox);
	if (!sti(InterfaceStates.Launched))
	{
		bool bFader = false;
		aref arFader;
		if (FindClass(arFader, "fader")) bFader = true;
		CoopNet_Log("give: the chest window did not open (fader " + bFader + ", reload fader " + IsEntity(reload_fader) + ", video " + g_ibVideoExecuting + ")");
		return;
	}
	bCoopNetGiveOpen = true;
	iCoopNetGiveTo = iBest;
	sCoopNetGiveToName = sBestName;
	CoopNet_Notice("Кооператив: положите в сундук то, что хотите передать игроку " + sBestName + ". Закроете окно - всё уйдёт ему");
}

// Every tick: the window closed, the box goes.
void CoopNet_GiveTick()
{
	if (!bCoopNetGiveOpen) return;
	if (sti(InterfaceStates.Launched) && CurrentInterface == INTERFACE_ITEMSBOX) return;
	bCoopNetGiveOpen = false;
	CoopNet_GiveSend();
}

void CoopNet_GiveSend()
{
	int iMoney = 0;
	if (CheckAttribute(&CoopNetGiveBox, "money")) iMoney = sti(CoopNetGiveBox.money);
	if (CheckAttribute(&CoopNetGiveBox, "items.gold"))
	{
		iMoney = iMoney + sti(CoopNetGiveBox.items.gold); // not turned into money yet
		DeleteAttribute(&CoopNetGiveBox, "items.gold");
	}
	aref arItems;
	makearef(arItems, CoopNetGiveBox.items);
	int iNum = GetAttributesNum(arItems);
	int iCount = 0;
	int i;
	for (i = 0; i < iNum; i++)
	{
		if (sti(GetAttributeValue(GetAttributeN(arItems, i))) > 0) iCount++;
	}
	if (iMoney <= 0 && iCount == 0) return;
	int iState = CoopNet_State();
	bool bThere = CheckAttribute(&CoopNetPlayers, "p" + iCoopNetGiveTo);
	if (iState != COOPNET_CONNECTED && iState != COOPNET_HOSTING) bThere = false;
	if (!bThere)
	{
		// nobody to take it: back to the hero
		if (iMoney > 0) AddMoneyToCharacter(pchar, iMoney);
		for (i = 0; i < iNum; i++)
		{
			aref arBack = GetAttributeN(arItems, i);
			int iBack = sti(GetAttributeValue(arBack));
			if (iBack > 0) TakeNItems(pchar, GetAttributeName(arBack), iBack);
		}
		DeleteAttribute(&CoopNetGiveBox, "");
		CoopNet_Notice("Кооператив: " + sCoopNetGiveToName + " уже ушёл, вещи остались у вас");
		return;
	}
	CoopNet_Begin(COOPNET_MSG_GIVE);
	CoopNet_PutInt(iCoopNetGiveTo);
	CoopNet_PutInt(CoopNet_MyId());
	CoopNet_PutString(GetFullName(pchar));
	CoopNet_PutInt(iMoney);
	CoopNet_PutInt(iCount);
	for (i = 0; i < iNum; i++)
	{
		aref arItem = GetAttributeN(arItems, i);
		int iQty = sti(GetAttributeValue(arItem));
		if (iQty <= 0) continue;
		CoopNet_PutString(GetAttributeName(arItem));
		CoopNet_PutInt(iQty);
	}
	if (iState == COOPNET_HOSTING) CoopNet_Send(iCoopNetGiveTo, COOPNET_RELIABLE);
	else CoopNet_Send(COOPNET_HOST_ID, COOPNET_RELIABLE);
	DeleteAttribute(&CoopNetGiveBox, "");
	CoopNet_Log("give: " + iMoney + " money and " + iCount + " kinds of items to player " + iCoopNetGiveTo);
	CoopNet_Notice("Кооператив: вы передали игроку " + sCoopNetGiveToName + " " + CoopNet_GiveWhat(iMoney, iCount));
}

string CoopNet_GiveWhat(int iMoney, int iCount)
{
	string sWhat = "";
	if (iMoney > 0) sWhat = iMoney + " пиастров";
	if (iCount > 0)
	{
		if (sWhat != "") sWhat = sWhat + " и ";
		sWhat = sWhat + "предметы (" + iCount + " видов)";
	}
	return sWhat;
}

void CoopNet_OnGive(int iFrom)
{
	int iTo = CoopNet_GetInt();
	int iBy = CoopNet_GetInt();
	string sName = CoopNet_GetString();
	int iMoney = CoopNet_GetInt();
	int iNum = CoopNet_GetInt();
	DeleteAttribute(&CoopNetGiveScratch, "");
	int i;
	for (i = 0; i < iNum; i++)
	{
		string sItem = CoopNet_GetString();
		string sKey = "i" + i;
		CoopNetGiveScratch.(sKey).id = sItem;
		CoopNetGiveScratch.(sKey).qty = CoopNet_GetInt();
	}
	if (CoopNet_State() == COOPNET_HOSTING && iBy != iFrom) return; // clients speak only for themselves
	if (iTo != CoopNet_MyId())
	{
		if (CoopNet_State() != COOPNET_HOSTING) return;
		CoopNet_Begin(COOPNET_MSG_GIVE);
		CoopNet_PutInt(iTo);
		CoopNet_PutInt(iBy);
		CoopNet_PutString(sName);
		CoopNet_PutInt(iMoney);
		CoopNet_PutInt(iNum);
		for (i = 0; i < iNum; i++)
		{
			sKey = "i" + i;
			CoopNet_PutString(CoopNetGiveScratch.(sKey).id);
			CoopNet_PutInt(sti(CoopNetGiveScratch.(sKey).qty));
		}
		CoopNet_Send(iTo, COOPNET_RELIABLE);
		return;
	}
	if (iMoney > 0) AddMoneyToCharacter(pchar, iMoney);
	for (i = 0; i < iNum; i++)
	{
		sKey = "i" + i;
		string sId = CoopNetGiveScratch.(sKey).id;
		if (GetItemIndex(sId) < 0)
		{
			CoopNet_Log("give: unknown item " + sId + " dropped");
			continue;
		}
		TakeNItems(pchar, sId, sti(CoopNetGiveScratch.(sKey).qty));
	}
	CoopNet_Log("give: got " + iMoney + " money and " + iNum + " kinds of items from player " + iBy);
	CoopNet_Notice("Кооператив: " + sName + " передал вам " + CoopNet_GiveWhat(iMoney, iNum));
}

// Returns true when the message was a give message.
bool CoopNet_GiveDispatch(int iType, int iFrom)
{
	if (iType != COOPNET_MSG_GIVE) return false;
	CoopNet_OnGive(iFrom);
	return true;
}

// COOPNET_GIVETEST=1 (with the land test): once both are in Bridgetown, the client puts 1234 gold
// and 2 bandages in the box for the host and closes it.
int iCoopNetGiveTestTicks = 0;

void CoopNet_TestGive()
{
	if (iCoopNetGiveTestTicks < 0 || CoopNet_GetEnv("COOPNET_GIVETEST") != "1") return;
	if (CoopNet_GetEnv("COOPNET_TEST") != "client" || CoopNet_LandHere() != "Bridgetown_town") return;
	iCoopNetGiveTestTicks++;
	if (iCoopNetGiveTestTicks == 360 && CurrentInterface == INTERFACE_CREATE_CHARACTER) Event("InterfaceBreak"); // a player closes it with Ok
	// the test hero walks the town with the hero menus of its start still marked open (a player closes them)
	if (iCoopNetGiveTestTicks == 380 && sti(InterfaceStates.Launched))
	{
		CoopNet_Log("test: give - interface " + CurrentInterface + " still marked open, cleared");
		InterfaceStates.Launched = false;
		UnloadSegment(Interfaces[INTERFACE_CHARACTER_SELECT].SectionName);
		UnloadSegment(Interfaces[INTERFACE_CREATE_CHARACTER].SectionName);
	}
	if (iCoopNetGiveTestTicks == 400)
	{
		// next to the host's puppet
		if (!CheckAttribute(&CoopNetLand, "p0.chr"))
		{
			CoopNet_Log("test: give - no host puppet here");
			iCoopNetGiveTestTicks = -1;
			return;
		}
		ref rHost = &Characters[sti(CoopNetLand.p0.chr)];
		float fX = 0.0;
		float fY = 0.0;
		float fZ = 0.0;
		SendMessage(rHost, "leee", MSG_CHARACTER_GETPOS, &fX, &fY, &fZ);
		TeleportCharacterToPos(pchar, fX + 1.0, fY, fZ);
		AddMoneyToCharacter(pchar, 5000);
		TakeNItems(pchar, "potion1", 3);
		CoopNet_Log("test: give - client money " + pchar.money + ", potion1 " + GetCharacterItem(pchar, "potion1"));
	}
	if (iCoopNetGiveTestTicks == 420)
	{
		CoopNet_GiveOffered();
		CoopNet_Log("test: give - box open " + bCoopNetGiveOpen);
	}
	if (iCoopNetGiveTestTicks == 460 && bCoopNetGiveOpen)
	{
		// as a player moving them over: the window keeps the hero's money as gold till it closes
		if (CheckAttribute(pchar, "items.gold")) pchar.items.gold = sti(pchar.items.gold) - 1234;
		else AddMoneyToCharacter(pchar, -1234);
		TakeNItems(pchar, "potion1", -2);
		CoopNetGiveBox.items.gold = 1234;
		CoopNetGiveBox.items.potion1 = 2;
		Event("InterfaceBreak");
	}
	if (iCoopNetGiveTestTicks == 500)
	{
		CoopNet_Log("test: give - client money " + pchar.money + ", potion1 " + GetCharacterItem(pchar, "potion1"));
		iCoopNetGiveTestTicks = -1;
	}
}
