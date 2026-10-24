// CoopNet rules: a player's death rolls the whole world back; a boarding pauses everybody else.
//
// Death (GameOver): a client tells the host and goes through the usual death screen to the main menu;
// the host, a few seconds later (or at once when it is the host that died), loads its latest save of this
// game: one written since the game was started, else the one it was loaded from (never an older one).
// Whatever save the host loads (also one it picks itself), everybody follows it: the host's world is
// sent again and every client gets its hero as that save has it (CoopNetGuests is saved with the
// host's game). A client the save doesn't know yet starts a new game and creates its hero.
//
// Boarding: every player's presence says whether it is boarding (bAbordageStarted). While anybody
// else boards, this player's game stands still (time scale 0, the shared clock stops too: boarding
// counts as a battle) until the boarding is over.

#define COOPNET_MSG_PLAYER_DIED		150		// client -> host: string name
#define COOPNET_MSG_WORLD_RELOADED	151		// host -> all: the host started or loaded a game
#define COOPNET_MSG_HOST_NOTICE		152		// host -> all: string text for the game screen
#define COOPNET_ROLLBACK_DELAY		4.0		// seconds between a client's death and the host's reload
#define COOPNET_MSG_SPEED		153		// int counter + 100 (TimeScaleCounter; negatives compare as unsigned), string by
#define COOPNET_SPEED_EVERY		5.0		// seconds between the host's reminders of the speed (for players who came in later)

float fCoopNetRollbackAt = -1.0;	// host: when to load the latest save
bool bCoopNetPausedForBoarding = false;
int iCoopNetSpeed = 0;			// the shared TimeScaleCounter
float fCoopNetSpeedSentAt = 0.0;
float fCoopNetSpeedOwnAt = -10.0;	// when this player last changed the speed itself
float fCoopNetSpeedWhyAt = 0.0;
float fCoopNetPauseNoticeAt = 0.0;

// ---- death and rollback -----------------------------------------------------------------------

// From GameOver (seadogs.c): true when the game over is replaced by the host's rollback.
bool CoopNet_GameOver(string sHow)
{
	int iState = CoopNet_State();
	if (iState == COOPNET_CONNECTED)
	{
		CoopNet_Log("rules: we died (" + sHow + "), the host rolls the world back");
		CoopNet_Begin(COOPNET_MSG_PLAYER_DIED);
		CoopNet_PutString(CoopNet_PeerName(CoopNet_MyId()));
		CoopNet_Send(COOPNET_HOST_ID, COOPNET_RELIABLE);
		// to the main menu as usual; the host's reload brings this player back in
		bCoopNetWorldInGame = false;
		return false;
	}
	if (iState == COOPNET_HOSTING)
	{
		CoopNet_Log("rules: the host died (" + sHow + ")");
		CoopNet_NoticeAll(" ооператив: хост погиб - откат мира к последнему сохранению");
		return CoopNet_Rollback();
	}
	return false;
}

void CoopNet_OnPlayerDied(int iFrom)
{
	string sName = CoopNet_GetString();
	if (CoopNet_State() != COOPNET_HOSTING) return;
	CoopNet_Log("rules: player " + iFrom + " '" + sName + "' died, rolling back");
	CoopNet_NoticeAll(" ооператив: " + sName + " погиб - откат мира к последнему сохранению");
	fCoopNetRollbackAt = CoopNet_Seconds() + COOPNET_ROLLBACK_DELAY;
}

// Host: loads the latest save. False when there is none.
bool CoopNet_Rollback()
{
	fCoopNetRollbackAt = -1.0;
	if (!CheckAttribute(&PlayerProfile, "name"))
	{
		CoopNet_NoticeAll(" ооператив: у хоста нет профил€ с сохранени€ми, откатывать некуда");
		return false;
	}
	string sDir = GetSaveDirectory() + PlayerProfile.name + "\";
	string sSave = CoopNet_RollbackSave(sDir); // saved since the game was started, or the one loaded
	if (sSave == "")
	{
		CoopNet_NoticeAll(" ооператив: у хоста нет сохранени€ этой игры, откатывать некуда - сохранитесь");
		return false;
	}
	CoopNet_Log("rules: loading " + sSave);
	SetEventHandler("evntLoad", "LoadGame", 0);
	PostEvent("evntLoad", 0, "s", sSave);
	return true;
}

// A notice for everybody (the host's own screen and every client's).
void CoopNet_NoticeAll(string sText)
{
	CoopNet_Notice(sText);
	if (CoopNet_State() != COOPNET_HOSTING) return;
	CoopNet_Begin(COOPNET_MSG_HOST_NOTICE);
	CoopNet_PutString(sText);
	CoopNet_Send(COOPNET_ALL, COOPNET_RELIABLE);
}

// Host, from CoopNet_WorldGameStarted: a new or loaded game; everybody takes its heroes from it.
void CoopNet_RulesWorldStarted()
{
	if (CoopNet_State() != COOPNET_HOSTING) return;
	CoopNet_Begin(COOPNET_MSG_WORLD_RELOADED);
	CoopNet_Send(COOPNET_ALL, COOPNET_RELIABLE);
}

// ---- boarding pause -------------------------------------------------------------------------------

void CoopNet_RulesTick()
{
	CoopNet_SpeedTick();
	int iState = CoopNet_State();
	if (fCoopNetRollbackAt > 0.0 && iState == COOPNET_HOSTING && CoopNet_Seconds() >= fCoopNetRollbackAt) CoopNet_Rollback();

	string sBoarder = "";
	if (iState == COOPNET_CONNECTED || iState == COOPNET_HOSTING)
	{
		if (!bAbordageStarted) sBoarder = CoopNet_RulesBoarder();
	}
	if (sBoarder == "")
	{
		if (bCoopNetPausedForBoarding)
		{
			bCoopNetPausedForBoarding = false;
			CoopNet_SpeedSet(); // the shared speed, not just 1
			CoopNet_Notice(" ооператив: абордаж окончен, игра продолжаетс€");
		}
		return;
	}
	// the menus set the time scale back when they close, so it is held at 0 every tick
	SetTimeScale(0.0);
	if (!bCoopNetPausedForBoarding || CoopNet_Seconds() > fCoopNetPauseNoticeAt)
	{
		bCoopNetPausedForBoarding = true;
		fCoopNetPauseNoticeAt = CoopNet_Seconds() + 10.0;
		CoopNet_Notice(" ооператив: " + sBoarder + " на абордаже - пауза");
	}
}

// The name of another player who is boarding now, "" when nobody is.
string CoopNet_RulesBoarder()
{
	int iMe = CoopNet_MyId();
	aref arPlayers;
	makearef(arPlayers, CoopNetPlayers);
	int iNum = GetAttributesNum(arPlayers);
	for (int i = 0; i < iNum; i++)
	{
		aref arP = GetAttributeN(arPlayers, i);
		if (sti(arP.id) == iMe || !CheckAttribute(arP, "busy")) continue;
		if (sti(arP.busy)) return arP.name;
	}
	return "";
}

// Returns true when the message was a rules message.
bool CoopNet_RulesDispatch(int iType, int iFrom)
{
	if (iType == COOPNET_MSG_SPEED)
	{
		CoopNet_OnSpeed(iFrom);
		return true;
	}
	if (iType < COOPNET_MSG_PLAYER_DIED || iType > COOPNET_MSG_HOST_NOTICE) return false;
	if (iType == COOPNET_MSG_PLAYER_DIED) CoopNet_OnPlayerDied(iFrom);
	if (iType == COOPNET_MSG_WORLD_RELOADED)
	{
		if (CoopNet_State() == COOPNET_CONNECTED && iFrom == COOPNET_HOST_ID) CoopNet_HeroWorldReloaded();
	}
	if (iType == COOPNET_MSG_HOST_NOTICE)
	{
		string sText = CoopNet_GetString();
		if (iFrom == COOPNET_HOST_ID) CoopNet_Notice(sText);
	}
	return true;
}

// ---- shared game speed ----------------------------------------------------------------------------
// One speed for everybody, as the clock: whoever speeds the game up or slows it down (the time keys,
// TimeScaleCounter) does it for all. Each player's own pauses stay its own: the speed is put back
// when no dialog, interface or boarding pause holds its game (those set the time scale to 0 and,
// closing, the counter back to 0).

// From the time keys (seadogs.c), once this player has changed TimeScaleCounter.
void CoopNet_SpeedChanged()
{
	int iState = CoopNet_State();
	if (iState != COOPNET_CONNECTED && iState != COOPNET_HOSTING) return;
	fCoopNetSpeedOwnAt = CoopNet_Seconds();
	iCoopNetSpeed = TimeScaleCounter;
	CoopNet_SpeedSend(CoopNet_PeerName(CoopNet_MyId()));
}

void CoopNet_SpeedSend(string sBy)
{
	CoopNet_Begin(COOPNET_MSG_SPEED);
	CoopNet_PutInt(iCoopNetSpeed + 100);
	CoopNet_PutString(sBy);
	if (CoopNet_State() == COOPNET_HOSTING) CoopNet_Send(COOPNET_ALL, COOPNET_RELIABLE);
	else CoopNet_Send(COOPNET_HOST_ID, COOPNET_RELIABLE);
	fCoopNetSpeedSentAt = CoopNet_Seconds();
}

void CoopNet_OnSpeed(int iFrom)
{
	int iSpeed = CoopNet_GetInt() - 100;
	string sBy = CoopNet_GetString();
	// a word sent before our own change got through would undo it for a moment
	if (CoopNet_Seconds() - fCoopNetSpeedOwnAt < 1.5 && sBy != CoopNet_PeerName(CoopNet_MyId())) return;
	bool bNews = iSpeed != iCoopNetSpeed;
	iCoopNetSpeed = iSpeed;
	if (CoopNet_State() == COOPNET_HOSTING) CoopNet_SpeedSend(sBy); // everybody, the sender too
	if (!bNews) return;
	float fScale = 1.0 + iSpeed * 0.25;
	CoopNet_Log("speed: x" + fScale + " by " + sBy);
	CoopNet_Notice(" ооператив: " + sBy + " - скорость игры x" + fScale);
	CoopNet_SpeedApply();
}

// The shared speed, unless something of this player's own holds the game.
void CoopNet_SpeedApply()
{
	if (TimeScaleCounter == iCoopNetSpeed) return;
	bool bHeld = dialogRun || bCoopNetPausedForBoarding;
	if (CheckAttribute(&InterfaceStates, "Launched") && sti(InterfaceStates.Launched)) bHeld = true;
	if (!bSeaActive && !bCoopNetInLocation && !IsEntity(&worldMap)) bHeld = true;
	if (bHeld)
	{
		if (CoopNet_Seconds() > fCoopNetSpeedWhyAt)
		{
			fCoopNetSpeedWhyAt = CoopNet_Seconds() + 5.0;
			string sWhy = "speed: x" + (1.0 + iCoopNetSpeed * 0.25) + " waits, dialog " + dialogRun + " boarding " + bCoopNetPausedForBoarding;
			if (CheckAttribute(&InterfaceStates, "Launched")) sWhy = sWhy + " interface " + InterfaceStates.Launched;
			CoopNet_Log(sWhy + " sea " + bSeaActive + " location " + bCoopNetInLocation + " map " + IsEntity(&worldMap) + ", counter " + TimeScaleCounter);
		}
		return;
	}
	CoopNet_SpeedSet();
}

void CoopNet_SpeedSet()
{
	CoopNet_Log("speed: this game at x" + (1.0 + iCoopNetSpeed * 0.25));
	TimeScaleCounter = iCoopNetSpeed;
	// as the time keys do (seadogs.c): the "TimeSpeed" mark tells R whether the game is sped up
	if (iCoopNetSpeed == 0)
	{
		SetTimeScale(1.0);
		if (IsPerkIntoList("TimeSpeed")) DelPerkFromActiveList("TimeSpeed");
	}
	else
	{
		SetTimeScale(1.0 + iCoopNetSpeed * 0.25);
		if (!IsPerkIntoList("TimeSpeed")) AddPerkToActiveList("TimeSpeed");
	}
}

void CoopNet_SpeedTick()
{
	int iState = CoopNet_State();
	if (iState != COOPNET_CONNECTED && iState != COOPNET_HOSTING) return;
	CoopNet_SpeedApply();
	if (iState == COOPNET_HOSTING && CoopNet_Seconds() - fCoopNetSpeedSentAt > COOPNET_SPEED_EVERY)
	{
		CoopNet_SpeedSend(CoopNet_PeerName(CoopNet_MyId()));
	}
}
