// CoopNet story: every player has its own quests, the consequences are shared.
//
// The stock characters (index below GlobalCharacters: governors, merchants, tavern keepers, the
// story's people) are the same in every game. One of them killed in one player's game is dead in
// everybody's: its death goes to the host, which keeps the list (a saved global, so it lasts with the
// host's world) and passes it on; a player joining later gets the list with the rest of the world.
// Who it is shown to: a character in the location a player walks in dies there as a scripted death,
// one elsewhere is just marked dead and taken off the map of people.

#define COOPNET_MSG_NPC_DEAD	166		// string id

object CoopNetDeadNpcs;	// host: id -> 1 for every stock character that died in a game of this session

// LAi_Character_Dead_Process (LAi_events.c): a character died in this game.
void CoopNet_StoryDeath(aref chr)
{
	int iState = CoopNet_State();
	if (iState != COOPNET_CONNECTED && iState != COOPNET_HOSTING) return;
	int iIdx = sti(chr.index);
	if (iIdx < 0 || iIdx >= GlobalCharacters || iIdx == nMainCharacterIndex) return;
	if (CheckAttribute(chr, "coopStoryDead")) return; // the death came from another game
	string sId = chr.id;
	if (sId == "" || sId == "0") return;
	chr.coopStoryDead = true;
	CoopNet_Log("story: " + sId + " died here");
	if (iState == COOPNET_HOSTING)
	{
		CoopNetDeadNpcs.(sId) = 1;
		CoopNet_StorySend(sId, COOPNET_ALL);
	}
	else CoopNet_StorySend(sId, COOPNET_HOST_ID);
}

void CoopNet_StorySend(string sId, int iTo)
{
	CoopNet_Begin(COOPNET_MSG_NPC_DEAD);
	CoopNet_PutString(sId);
	CoopNet_Send(iTo, COOPNET_RELIABLE);
}

// The host's world for a joining player: every death so far.
void CoopNet_StorySendAll(int iTo)
{
	aref arAll;
	makearef(arAll, CoopNetDeadNpcs);
	int iNum = GetAttributesNum(arAll);
	for (int i = 0; i < iNum; i++)
	{
		CoopNet_StorySend(GetAttributeName(GetAttributeN(arAll, i)), iTo);
	}
	if (iNum > 0) CoopNet_Log("story: sent " + iNum + " deaths to player " + iTo);
}

void CoopNet_OnStoryDeath(int iFrom)
{
	string sId = CoopNet_GetString();
	if (CoopNet_State() == COOPNET_HOSTING)
	{
		if (CheckAttribute(&CoopNetDeadNpcs, sId)) return; // known already
		CoopNetDeadNpcs.(sId) = 1;
		CoopNet_StorySend(sId, COOPNET_ALL);
	}
	CoopNet_StoryApply(sId);
}

void CoopNet_StoryApply(string sId)
{
	int iIdx = GetCharacterIndex(sId);
	if (iIdx < 0 || iIdx >= GlobalCharacters || iIdx == nMainCharacterIndex) return;
	ref chr = &Characters[iIdx];
	if (CheckAttribute(chr, "coopStoryDead")) return;
	chr.coopStoryDead = true;
	string sName = GetFullName(chr);
	string sHere = CoopNet_LandHere();
	if (sHere != "" && IsEntity(chr) && CheckAttribute(chr, "location") && chr.location == sHere && !LAi_IsDead(chr))
	{
		LAi_KillCharacter(chr);
		CoopNet_Log("story: " + sId + " killed here, it died in another game");
	}
	else
	{
		chr.chr_ai.hp = 0.0;
		if (IsEntity(chr)) ChangeCharacterAddressGroup(chr, "none", "", "");
		chr.location = "none";
		CoopNet_Log("story: " + sId + " marked dead, it died in another game");
	}
	if (bCoopNetWorldSynced || CoopNet_State() == COOPNET_HOSTING) CoopNet_Notice(" ооператив: " + sName + " погиб в игре другого игрока");
}

// Returns true when the message was a story message.
bool CoopNet_StoryDispatch(int iType, int iFrom)
{
	if (iType != COOPNET_MSG_NPC_DEAD) return false;
	if (!bCoopNetWorldInGame) return true; // no world here yet: the list comes with it
	CoopNet_OnStoryDeath(iFrom);
	return true;
}

// COOPNET_STORYTEST=1 (with the land test): the host kills the waitress once it is in the tavern;
// the client, still in the town, logs whether she is dead in its game too.
int iCoopNetStoryTestTicks = 0;

void CoopNet_TestStory()
{
	if (iCoopNetStoryTestTicks < 0 || CoopNet_GetEnv("COOPNET_STORYTEST") != "1") return;
	if (CoopNet_LandHere() == "") return;
	iCoopNetStoryTestTicks++;
	ref rWaitress = CharacterFromID("Bridgetown_waitress");
	bool bClient = CoopNet_GetEnv("COOPNET_TEST") == "client";
	if (!bClient && CoopNet_LandHere() == "Bridgetown_tavern")
	{
		CoopNet_Log("test: story - host kills " + rWaitress.id + " (index " + rWaitress.index + " of " + GlobalCharacters + " stock, here " + IsEntity(rWaitress) + ")");
		LAi_KillCharacter(rWaitress);
		iCoopNetStoryTestTicks = -1;
	}
	if (bClient && iCoopNetStoryTestTicks == 2000)
	{
		CoopNet_Log("test: story - client sees " + rWaitress.id + " dead " + LAi_IsDead(rWaitress) + ", location " + rWaitress.location);
		iCoopNetStoryTestTicks = -1;
	}
}
