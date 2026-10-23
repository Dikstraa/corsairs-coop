// CoopNet land: players in the same location (a town, a jungle, a house) see each other.
//
// Every player on land (not at sea, so not in a cabin, on a deck or boarding) sends where its hero
// stands four times a second: location, position, heading, speed, whether the blade is out, and its
// look (model, animation, name). The host passes it on to everybody. Locations are the same for
// everybody, so positions are used as they come.
//
// Another player in this player's location is a "puppet": an NPC with that player's model (actor
// type, immortal, no dialog) that runs or walks to where the player will be a moment later, jumps
// there when it falls too far behind and turns to the player's heading when it stands. A puppet is
// sent away (location "none") when its player leaves or this player does, so it is never left
// behind in a location the game loads again.

#define COOPNET_MSG_LAND		160		// int id, string location, float x, y, z, ay, vx, vz, int flags, string model, string ani, string sex, string name
#define COOPNET_LAND_FIGHT		1		// flags: the blade is out (the only flag so far)
#define COOPNET_LAND_TICKS		5		// report every 5 ticks = 0.25 s
#define COOPNET_LAND_STALE		3.0		// seconds without a report before a player has left
#define COOPNET_LAND_LEAD		0.5		// seconds ahead a puppet runs to
#define COOPNET_LAND_JUMP		6.0		// metres behind before a puppet is put in place
#define COOPNET_LAND_RUN		2.5		// metres per second: faster runs, slower walks
#define COOPNET_LAND_STILL		0.3		// metres per second: slower stands
#define COOPNET_LAND_NEAR		0.5		// metres: a standing puppet this close stays put

object CoopNetLand;	// p<id>: id, loc, x, y, z, ay, vx, vz, flags, model, ani, sex, name, at; puppet: chr, chrModel, gx, gz, fight
int iCoopNetLandTicks = 0;
float fCoopNetLandFromX = 0.0;	// our position at the last report, for our speed
float fCoopNetLandFromZ = 0.0;
float fCoopNetLandFromAt = -1.0;
string sCoopNetLandFromLoc = "";
float fCoopNetLandLogAt = 0.0;

void CoopNet_LandTick()
{
	int iState = CoopNet_State();
	if (iState != COOPNET_CONNECTED && iState != COOPNET_HOSTING) return;
	iCoopNetLandTicks++;
	if (iCoopNetLandTicks < COOPNET_LAND_TICKS) return;
	iCoopNetLandTicks = 0;
	if (CoopNet_LandHere() != "") CoopNet_LandReport();
	CoopNet_LandUpdatePuppets();
	CoopNet_LandLog();
}

// The location this player walks in, "" when it is not on land.
string CoopNet_LandHere()
{
	if (!bCoopNetInLocation || bSeaActive || IsEntity(&worldMap)) return "";
	if (iCoopNetMode != COOPNET_MODE_LAND) return "";
	if (!CheckAttribute(pchar, "location")) return "";
	if (pchar.location == "SelectMainCharacter_Cabine") return ""; // still choosing a hero
	if (!CheckAttribute(pchar, "model") || pchar.model == "" || pchar.model == "none") return "";
	return pchar.location;
}

void CoopNet_LandReport()
{
	float fX = 0.0;
	float fY = 0.0;
	float fZ = 0.0;
	float fAY = 0.0;
	if (SendMessage(pchar, "leee", MSG_CHARACTER_GETPOS, &fX, &fY, &fZ) == 0) return;
	SendMessage(pchar, "le", MSG_CHARACTER_GETAY, &fAY);
	string sLoc = pchar.location;
	float fNow = CoopNet_Seconds();
	float fVX = 0.0;
	float fVZ = 0.0;
	if (sLoc == sCoopNetLandFromLoc && fCoopNetLandFromAt > 0.0 && fNow - fCoopNetLandFromAt > 0.05)
	{
		fVX = (fX - fCoopNetLandFromX) / (fNow - fCoopNetLandFromAt);
		fVZ = (fZ - fCoopNetLandFromZ) / (fNow - fCoopNetLandFromAt);
	}
	fCoopNetLandFromX = fX;
	fCoopNetLandFromZ = fZ;
	fCoopNetLandFromAt = fNow;
	sCoopNetLandFromLoc = sLoc;
	int iFlags = 0;
	if (SendMessage(pchar, "ls", MSG_CHARACTER_EX_MSG, "IsFightMode") != 0) iFlags = COOPNET_LAND_FIGHT;
	string sAni = "man";
	if (CheckAttribute(pchar, "model.animation")) sAni = pchar.model.animation;
	string sSex = "man";
	if (CheckAttribute(pchar, "sex")) sSex = pchar.sex;

	CoopNet_Begin(COOPNET_MSG_LAND);
	CoopNet_PutInt(CoopNet_MyId());
	CoopNet_PutString(sLoc);
	CoopNet_PutFloat(fX);
	CoopNet_PutFloat(fY);
	CoopNet_PutFloat(fZ);
	CoopNet_PutFloat(fAY);
	CoopNet_PutFloat(fVX);
	CoopNet_PutFloat(fVZ);
	CoopNet_PutInt(iFlags);
	CoopNet_PutString(pchar.model);
	CoopNet_PutString(sAni);
	CoopNet_PutString(sSex);
	CoopNet_PutString(GetFullName(pchar));
	if (CoopNet_State() == COOPNET_HOSTING) CoopNet_Send(COOPNET_ALL, COOPNET_UNRELIABLE);
	else CoopNet_Send(COOPNET_HOST_ID, COOPNET_UNRELIABLE);
}

void CoopNet_OnLand(int iFrom)
{
	int iId = CoopNet_GetInt();
	string sLoc = CoopNet_GetString();
	float fX = CoopNet_GetFloat();
	float fY = CoopNet_GetFloat();
	float fZ = CoopNet_GetFloat();
	float fAY = CoopNet_GetFloat();
	float fVX = CoopNet_GetFloat();
	float fVZ = CoopNet_GetFloat();
	int iFlags = CoopNet_GetInt();
	string sModel = CoopNet_GetString();
	string sAni = CoopNet_GetString();
	string sSex = CoopNet_GetString();
	string sName = CoopNet_GetString();
	if (iId == CoopNet_MyId()) return;
	if (CoopNet_State() == COOPNET_HOSTING)
	{
		if (iId != iFrom) return; // clients speak only for themselves
		CoopNet_Begin(COOPNET_MSG_LAND);
		CoopNet_PutInt(iId);
		CoopNet_PutString(sLoc);
		CoopNet_PutFloat(fX);
		CoopNet_PutFloat(fY);
		CoopNet_PutFloat(fZ);
		CoopNet_PutFloat(fAY);
		CoopNet_PutFloat(fVX);
		CoopNet_PutFloat(fVZ);
		CoopNet_PutInt(iFlags);
		CoopNet_PutString(sModel);
		CoopNet_PutString(sAni);
		CoopNet_PutString(sSex);
		CoopNet_PutString(sName);
		CoopNet_Send(COOPNET_ALL, COOPNET_UNRELIABLE);
	}
	string sKey = "p" + iId;
	if (!CheckAttribute(&CoopNetLand, sKey + ".loc") || CoopNetLand.(sKey).loc != sLoc)
	{
		CoopNet_Log("land: player " + iId + " '" + sName + "' in " + sLoc);
	}
	CoopNetLand.(sKey).id = iId;
	CoopNetLand.(sKey).loc = sLoc;
	CoopNetLand.(sKey).x = fX;
	CoopNetLand.(sKey).y = fY;
	CoopNetLand.(sKey).z = fZ;
	CoopNetLand.(sKey).ay = fAY;
	CoopNetLand.(sKey).vx = fVX;
	CoopNetLand.(sKey).vz = fVZ;
	CoopNetLand.(sKey).flags = iFlags;
	CoopNetLand.(sKey).model = sModel;
	CoopNetLand.(sKey).ani = sAni;
	CoopNetLand.(sKey).sex = sSex;
	CoopNetLand.(sKey).name = sName;
	CoopNetLand.(sKey).at = CoopNet_Seconds();
}

// ---- puppets ----------------------------------------------------------------------------

void CoopNet_LandUpdatePuppets()
{
	string sHere = CoopNet_LandHere();
	float fNow = CoopNet_Seconds();
	aref arAll;
	makearef(arAll, CoopNetLand);
	int iNum = GetAttributesNum(arAll);
	for (int i = iNum - 1; i >= 0; i--)
	{
		aref arP = GetAttributeN(arAll, i);
		bool bStale = fNow - stf(arP.at) > COOPNET_LAND_STALE;
		if (bStale || sHere == "" || arP.loc != sHere)
		{
			CoopNet_LandRemovePuppet(arP);
			if (bStale) DeleteAttribute(arAll, GetAttributeName(arP));
			continue;
		}
		if (CheckAttribute(arP, "chr"))
		{
			if (arP.chrModel != arP.model) CoopNet_LandRemovePuppet(arP); // changed clothes: made anew
		}
		if (!CheckAttribute(arP, "chr"))
		{
			if (!CoopNet_LandCreatePuppet(arP)) continue;
		}
		CoopNet_LandSteer(arP);
	}
}

bool CoopNet_LandCreatePuppet(aref arP)
{
	string sId = "CoopLand_" + arP.id;
	int iChr = NPC_GenerateCharacter(sId, arP.model, arP.sex, arP.ani, 1, PIRATE, -1, false);
	if (iChr < 0)
	{
		CoopNet_Log("land: no free character for player " + arP.id);
		return false;
	}
	ref rChr = &Characters[iChr];
	rChr.name = arP.name;
	rChr.lastname = "";
	rChr.coopLand = arP.id;
	DeleteAttribute(rChr, "Dialog");
	rChr.Dialog.Filename = "";
	// logged in where this player came in, then put in place
	string sGroup = "reload";
	string sLocator = "";
	if (CheckAttribute(pchar, "location.group")) sGroup = pchar.location.group;
	if (CheckAttribute(pchar, "location.locator")) sLocator = pchar.location.locator;
	ChangeCharacterAddressGroup(rChr, pchar.location, sGroup, sLocator);
	LAi_SetActorType(rChr);
	LAi_SetImmortal(rChr, true);
	LAi_CharacterDisableDialog(rChr);
	TeleportCharacterToPosAy(rChr, stf(arP.x), stf(arP.y), stf(arP.z), stf(arP.ay));
	arP.chr = iChr;
	arP.chrModel = arP.model;
	arP.fight = false;
	arP.gx = arP.x;
	arP.gz = arP.z;
	CoopNet_Log("land: player " + arP.id + " '" + arP.name + "' as " + sId + " (" + arP.model + ") at " + arP.x + " " + arP.z);
	return true;
}

void CoopNet_LandRemovePuppet(aref arP)
{
	if (!CheckAttribute(arP, "chr")) return;
	int iChr = sti(arP.chr);
	DeleteAttribute(arP, "chr");
	ref rChr = &Characters[iChr];
	ChangeCharacterAddressGroup(rChr, "none", "", "");
	rChr.location = "none";
	CoopNet_Log("land: player " + arP.id + " puppet removed");
}

// Every puppet leaves when this player's location goes away (before it can be saved with it).
void CoopNet_LandForgetScene()
{
	aref arAll;
	makearef(arAll, CoopNetLand);
	int iNum = GetAttributesNum(arAll);
	for (int i = 0; i < iNum; i++)
	{
		aref arP = GetAttributeN(arAll, i);
		if (!CheckAttribute(arP, "chr")) continue;
		int iChr = sti(arP.chr);
		DeleteAttribute(arP, "chr");
		Characters[iChr].location = "none";
	}
}

void CoopNet_LandSteer(aref arP)
{
	ref rChr = &Characters[sti(arP.chr)];
	float fAge = CoopNet_Seconds() - stf(arP.at);
	float fVX = stf(arP.vx);
	float fVZ = stf(arP.vz);
	float fSpeed = sqrt(fVX * fVX + fVZ * fVZ);
	float fGX = stf(arP.x) + fVX * (fAge + COOPNET_LAND_LEAD);
	float fGZ = stf(arP.z) + fVZ * (fAge + COOPNET_LAND_LEAD);
	float fY = stf(arP.y);

	bool bFight = sti(arP.flags) == COOPNET_LAND_FIGHT;
	if (bFight != sti(arP.fight))
	{
		arP.fight = bFight;
		LAi_SetFightMode(rChr, bFight);
	}

	float fX = 0.0;
	float fCY = 0.0;
	float fZ = 0.0;
	if (SendMessage(rChr, "leee", MSG_CHARACTER_GETPOS, &fX, &fCY, &fZ) == 0) return;
	float fDX = stf(arP.x) - fX;
	float fDZ = stf(arP.z) - fZ;
	float fBehind = sqrt(fDX * fDX + fDZ * fDZ);
	if (fBehind > COOPNET_LAND_JUMP)
	{
		TeleportCharacterToPosAy(rChr, stf(arP.x), fY, stf(arP.z), stf(arP.ay));
		arP.gx = arP.x;
		arP.gz = arP.z;
		SetCharacterTask_Stay(rChr);
		return;
	}
	if (fSpeed < COOPNET_LAND_STILL)
	{
		if (fBehind > COOPNET_LAND_NEAR)
		{
			SendMessage(rChr, "lsfff", MSG_NPCHARACTER_SETTASK, "Goto point", stf(arP.x), fY, stf(arP.z));
		}
		else
		{
			CharacterTurnAy(rChr, stf(arP.ay));
		}
		return;
	}
	string sTask = "Goto point";
	if (fSpeed > COOPNET_LAND_RUN) sTask = "Runto point";
	SendMessage(rChr, "lsfff", MSG_NPCHARACTER_SETTASK, sTask, fGX, fY, fGZ);
	arP.gx = fGX;
	arP.gz = fGZ;
}

// Every 10 s while a puppet is out: how far behind its player it walks.
void CoopNet_LandLog()
{
	if (CoopNet_Seconds() < fCoopNetLandLogAt) return;
	aref arAll;
	makearef(arAll, CoopNetLand);
	int iNum = GetAttributesNum(arAll);
	string sLine = "";
	for (int i = 0; i < iNum; i++)
	{
		aref arP = GetAttributeN(arAll, i);
		if (!CheckAttribute(arP, "chr")) continue;
		float fX = 0.0;
		float fY = 0.0;
		float fZ = 0.0;
		SendMessage(&Characters[sti(arP.chr)], "leee", MSG_CHARACTER_GETPOS, &fX, &fY, &fZ);
		float fDX = stf(arP.x) - fX;
		float fDZ = stf(arP.z) - fZ;
		sLine = sLine + "; player " + arP.id + " in " + arP.loc + " at " + arP.x + " " + arP.z + ", puppet " + sqrt(fDX * fDX + fDZ * fDZ) + " off";
	}
	if (sLine == "") return;
	fCoopNetLandLogAt = CoopNet_Seconds() + 10.0;
	CoopNet_Log("land: me in " + CoopNet_LandHere() + sLine);
}

// Returns true when the message was a land message.
bool CoopNet_LandDispatch(int iType, int iFrom)
{
	if (iType != COOPNET_MSG_LAND) return false;
	CoopNet_OnLand(iFrom);
	return true;
}

// ---- test -----------------------------------------------------------------------------------

int iCoopNetLandTestStep = 0;
int iCoopNetLandTestRoomTicks = 0;

// COOPNET_LANDTEST=1: both go from the hero room to Bridgetown. 10 s after reaching land the client's hero
// runs 10 m (moved 1 m every report, ~4 m/s), stands, runs back, stands, then draws its blade.
void CoopNet_TestLand()
{
	if (iCoopNetLandTestStep < 0 || CoopNet_GetEnv("COOPNET_LANDTEST") != "1") return;
	if (bCoopNetInLocation && !bSeaActive && CheckAttribute(pchar, "location") && iCoopNetLandTicks == 0)
	{
		if (pchar.location == "SelectMainCharacter_Cabine" && bCoopNetWorldInGame)
		{
			iCoopNetLandTestRoomTicks++;
			if (iCoopNetLandTestRoomTicks == 20) DoReloadCharacterToLocation("Bridgetown_town", "reload", "reload1");
		}
	}
	if (CoopNet_GetEnv("COOPNET_TEST") != "client") return;
	if (CoopNet_LandHere() == "" || iCoopNetLandTicks != 0) return;
	iCoopNetLandTestStep++;
	int iStep = iCoopNetLandTestStep - 40;
	if (iStep < 0) return;
	float fX = 0.0;
	float fY = 0.0;
	float fZ = 0.0;
	SendMessage(pchar, "leee", MSG_CHARACTER_GETPOS, &fX, &fY, &fZ);
	if (iStep == 0) CoopNet_Log("test: land run from " + fX + " " + fZ);
	if (iStep < 10) TeleportCharacterToPos(pchar, fX + 1.0, fY, fZ);
	if (iStep >= 30 && iStep < 40) TeleportCharacterToPos(pchar, fX - 1.0, fY, fZ);
	if (iStep == 60)
	{
		CoopNet_Log("test: land blade out");
		LAi_SetFightMode(pchar, true);
	}
	if (iStep == 80) iCoopNetLandTestStep = -1;
}
