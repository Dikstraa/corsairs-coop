// CoopNet land: players in the same location (a town, a jungle, a house) see each other and the
// same people around them.
//
// Every player on land (not at sea, so not in a cabin, on a deck or boarding) sends where its hero
// stands four times a second: location, position, heading, speed, whether the blade is out, its look
// (model, animation, name) and how long it has been in that location. The host passes it on to
// everybody. Locations are the same for everybody, so positions are used as they come.
//
// Another player in this player's location is a "puppet": an NPC with that player's model (actor
// type, immortal, no dialog) that runs or walks to where the player will be a moment later, jumps
// there when it falls too far behind and turns to the player's heading when it stands.
//
// People: the stock characters (index below GlobalCharacters: merchants, governors, ...) are the
// same in every game and stay as they are. Everybody else in a location (citizens, soldiers,
// bandits, monsters, quest people) was made by the game that loaded it, so the player who has been
// there longest "owns" the location: its people are the real ones. It sends them twice a second
// and everybody else there sees them as puppets too (walking, blade, death); their own made-up people
// are hidden (location "none") while they are not the owner and come back when they leave or become
// the owner. Officers are never hidden: every player sends its own, so everybody sees everybody's.
// Puppets are sent away when their player leaves or this player does, so the game never logs them
// in again when it loads that location.

#define COOPNET_MSG_LAND		160		// int id, string location, float x, y, z, ay, vx, vz, int flags, string model, string ani, string sex, string name, float here
#define COOPNET_MSG_LAND_PEOPLE	161		// int id, string location, int n, n x (int idx, string model, string ani, string sex, float x, y, z, ay, vx, vz, int flags)
#define COOPNET_LAND_FIGHT		1		// flags: the blade is out
#define COOPNET_LAND_DEAD		2		// flags: dead (with the blade: 3)
#define COOPNET_LAND_TICKS		5		// report every 5 ticks = 0.25 s
#define COOPNET_LAND_PEOPLE_EVERY	2	// people every 2nd report = 0.5 s
#define COOPNET_LAND_PER_MSG	12		// people per message
#define COOPNET_LAND_STALE		3.0		// seconds without a report before a player has left
#define COOPNET_LAND_PEOPLE_STALE	1.6	// seconds without news before a person is gone
#define COOPNET_LAND_LEAD		0.5		// seconds ahead a puppet runs to
#define COOPNET_LAND_JUMP		6.0		// metres behind before a puppet is put in place
#define COOPNET_LAND_RUN		2.5		// metres per second: faster runs, slower walks
#define COOPNET_LAND_STILL		0.3		// metres per second: slower stands
#define COOPNET_LAND_NEAR		0.5		// metres: a standing puppet this close stays put

object CoopNetLand;			// p<id>: id, loc, x, y, z, ay, vx, vz, flags, model, ani, sex, name, here, at; puppet: chr, chrModel, fight, dead
object CoopNetLandPeople;	// o<owner>_<idx>: owner, idx, loc, same fields as a player
object CoopNetLandSeen;		// c<idx>: x, z, at - our people's last positions, for their speed
object CoopNetLandFree;		// c<idx>: puppet characters to give back once their location is gone
object CoopNetLandHidden;	// c<idx>: our people hidden while another player owns the location
int iCoopNetLandTicks = 0;
int iCoopNetLandPeopleTicks = 0;
float fCoopNetLandFromX = 0.0;	// our position at the last report, for our speed
float fCoopNetLandFromZ = 0.0;
float fCoopNetLandFromAt = -1.0;
string sCoopNetLandFromLoc = "";
string sCoopNetLandArrivedLoc = "";	// where we are and since when
float fCoopNetLandArrivedAt = 0.0;
bool bCoopNetLandOwner = true;
float fCoopNetLandLogAt = 0.0;
int iCoopNetLandList[64];			// characters picked from LAi_loginedcharacters
int iCoopNetLandListNum = 0;

void CoopNet_LandTick()
{
	int iState = CoopNet_State();
	if (iState != COOPNET_CONNECTED && iState != COOPNET_HOSTING) return;
	iCoopNetLandTicks++;
	if (iCoopNetLandTicks < COOPNET_LAND_TICKS) return;
	iCoopNetLandTicks = 0;
	string sHere = CoopNet_LandHere();
	if (sHere != sCoopNetLandArrivedLoc)
	{
		sCoopNetLandArrivedLoc = sHere;
		fCoopNetLandArrivedAt = CoopNet_Seconds();
	}
	CoopNet_LandFreeSlots();
	if (sHere != "")
	{
		CoopNet_LandReport();
		CoopNet_LandOwnership(sHere);
		iCoopNetLandPeopleTicks++;
		if (iCoopNetLandPeopleTicks >= COOPNET_LAND_PEOPLE_EVERY)
		{
			iCoopNetLandPeopleTicks = 0;
			if (CoopNet_LandOthersHere(sHere)) CoopNet_LandSendPeople(sHere);
		}
	}
	CoopNet_LandUpdatePuppets(sHere);
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

// ---- players --------------------------------------------------------------------------------

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
	CoopNet_PutFloat(fNow - fCoopNetLandArrivedAt);
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
	float fHere = CoopNet_GetFloat();
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
		CoopNet_PutFloat(fHere);
		CoopNet_Send(COOPNET_ALL, COOPNET_UNRELIABLE);
	}
	string sKey = "p" + iId;
	if (!CheckAttribute(&CoopNetLand, sKey + ".loc") || CoopNetLand.(sKey).loc != sLoc)
	{
		CoopNet_Log("land: player " + iId + " '" + sName + "' in " + sLoc);
	}
	CoopNetLand.(sKey).id = iId;
	CoopNetLand.(sKey).loc = sLoc;
	CoopNetLand.(sKey).name = sName;
	CoopNetLand.(sKey).here = fHere;
	aref arP;
	makearef(arP, CoopNetLand.(sKey));
	CoopNet_LandStore(arP, fX, fY, fZ, fAY, fVX, fVZ, iFlags, sModel, sAni, sSex);
}

void CoopNet_LandStore(aref arE, float fX, float fY, float fZ, float fAY, float fVX, float fVZ, int iFlags, string sModel, string sAni, string sSex)
{
	arE.x = fX;
	arE.y = fY;
	arE.z = fZ;
	arE.ay = fAY;
	arE.vx = fVX;
	arE.vz = fVZ;
	arE.flags = iFlags;
	arE.model = sModel;
	arE.ani = sAni;
	arE.sex = sSex;
	arE.at = CoopNet_Seconds();
}

// True when another player walks in this location now.
bool CoopNet_LandOthersHere(string sHere)
{
	float fNow = CoopNet_Seconds();
	aref arAll;
	makearef(arAll, CoopNetLand);
	int iNum = GetAttributesNum(arAll);
	for (int i = 0; i < iNum; i++)
	{
		aref arP = GetAttributeN(arAll, i);
		if (arP.loc == sHere && fNow - stf(arP.at) <= COOPNET_LAND_STALE) return true;
	}
	return false;
}

// The owner of a location is the player who has been there longest (the lower id on a tie).
void CoopNet_LandOwnership(string sHere)
{
	float fNow = CoopNet_Seconds();
	float fMine = fNow - fCoopNetLandArrivedAt;
	bool bOwner = true;
	string sOwner = "";
	aref arAll;
	makearef(arAll, CoopNetLand);
	int iNum = GetAttributesNum(arAll);
	for (int i = 0; i < iNum; i++)
	{
		aref arP = GetAttributeN(arAll, i);
		if (arP.loc != sHere || fNow - stf(arP.at) > COOPNET_LAND_STALE) continue;
		float fTheirs = stf(arP.here) + fNow - stf(arP.at);
		bool bTheirs = false;
		if (fTheirs > fMine + 0.5) bTheirs = true;
		if (fTheirs >= fMine - 0.5 && fTheirs <= fMine + 0.5 && sti(arP.id) < CoopNet_MyId()) bTheirs = true;
		if (bTheirs)
		{
			bOwner = false;
			sOwner = arP.name;
		}
	}
	if (bOwner != bCoopNetLandOwner)
	{
		bCoopNetLandOwner = bOwner;
		if (bOwner) CoopNet_Log("land: we own " + sHere + " now, our people come back");
		else CoopNet_Log("land: " + sOwner + " owns " + sHere + ", our people step aside");
		if (bOwner) CoopNet_LandRestoreHidden(true);
	}
	if (!bOwner) CoopNet_LandHideOurPeople();
}

// ---- our people -------------------------------------------------------------------------------

// Our people in this location worth sending: everybody made by this game (all of them as the
// owner, only the officers otherwise), not the stock characters, not the hero, not puppets.
void CoopNet_LandPickPeople(bool bAll)
{
	iCoopNetLandListNum = 0;
	for (int i = 0; i < LAi_numloginedcharacters; i++)
	{
		int iIdx = LAi_loginedcharacters[i];
		if (iIdx < GlobalCharacters || iIdx == nMainCharacterIndex) continue;
		ref rChr = &Characters[iIdx];
		if (CheckAttribute(rChr, "coopLand") || CheckAttribute(rChr, "coopLandHidden")) continue;
		if (!bAll && !CoopNet_LandIsOfficer(rChr)) continue;
		if (iCoopNetLandListNum >= 64) break;
		iCoopNetLandList[iCoopNetLandListNum] = iIdx;
		iCoopNetLandListNum++;
	}
}

bool CoopNet_LandIsOfficer(ref rChr)
{
	if (!CheckAttribute(rChr, "chr_ai.type")) return false;
	return rChr.chr_ai.type == "officer";
}

void CoopNet_LandSendPeople(string sHere)
{
	CoopNet_LandPickPeople(bCoopNetLandOwner);
	float fNow = CoopNet_Seconds();
	int iNum = iCoopNetLandListNum;
	int iSent = 0;
	while (iSent < iNum || iSent == 0)
	{
		int iCount = iNum - iSent;
		if (iCount > COOPNET_LAND_PER_MSG) iCount = COOPNET_LAND_PER_MSG;
		CoopNet_Begin(COOPNET_MSG_LAND_PEOPLE);
		CoopNet_PutInt(CoopNet_MyId());
		CoopNet_PutString(sHere);
		CoopNet_PutInt(iCount);
		for (int k = 0; k < iCount; k++)
		{
			int iIdx = iCoopNetLandList[iSent + k];
			ref rChr = &Characters[iIdx];
			float fX = 0.0;
			float fY = 0.0;
			float fZ = 0.0;
			float fAY = 0.0;
			SendMessage(rChr, "leee", MSG_CHARACTER_GETPOS, &fX, &fY, &fZ);
			SendMessage(rChr, "le", MSG_CHARACTER_GETAY, &fAY);
			string sC = "c" + iIdx;
			float fVX = 0.0;
			float fVZ = 0.0;
			if (CheckAttribute(&CoopNetLandSeen, sC + ".at"))
			{
				float fDt = fNow - stf(CoopNetLandSeen.(sC).at);
				if (fDt > 0.05 && fDt < 2.0)
				{
					fVX = (fX - stf(CoopNetLandSeen.(sC).x)) / fDt;
					fVZ = (fZ - stf(CoopNetLandSeen.(sC).z)) / fDt;
				}
			}
			CoopNetLandSeen.(sC).x = fX;
			CoopNetLandSeen.(sC).z = fZ;
			CoopNetLandSeen.(sC).at = fNow;
			int iFlags = 0;
			if (SendMessage(rChr, "ls", MSG_CHARACTER_EX_MSG, "IsFightMode") != 0) iFlags = COOPNET_LAND_FIGHT;
			if (LAi_IsDead(rChr)) iFlags = iFlags + COOPNET_LAND_DEAD;
			string sAni = "man";
			if (CheckAttribute(rChr, "model.animation")) sAni = rChr.model.animation;
			string sSex = "man";
			if (CheckAttribute(rChr, "sex")) sSex = rChr.sex;
			CoopNet_PutInt(iIdx);
			CoopNet_PutString(rChr.model);
			CoopNet_PutString(sAni);
			CoopNet_PutString(sSex);
			CoopNet_PutFloat(fX);
			CoopNet_PutFloat(fY);
			CoopNet_PutFloat(fZ);
			CoopNet_PutFloat(fAY);
			CoopNet_PutFloat(fVX);
			CoopNet_PutFloat(fVZ);
			CoopNet_PutInt(iFlags);
		}
		if (CoopNet_State() == COOPNET_HOSTING) CoopNet_Send(COOPNET_ALL, COOPNET_UNRELIABLE);
		else CoopNet_Send(COOPNET_HOST_ID, COOPNET_UNRELIABLE);
		iSent = iSent + iCount;
		if (iCount == 0) break;
	}
}

// Not the owner: our own made-up people (not the officers) leave the location for now.
void CoopNet_LandHideOurPeople()
{
	iCoopNetLandListNum = 0;
	for (int i = 0; i < LAi_numloginedcharacters; i++)
	{
		int iIdx = LAi_loginedcharacters[i];
		if (iIdx < GlobalCharacters || iIdx == nMainCharacterIndex) continue;
		ref rChr = &Characters[iIdx];
		if (CheckAttribute(rChr, "coopLand") || CoopNet_LandIsOfficer(rChr)) continue;
		if (CheckAttribute(rChr, "coopLandHidden")) continue;
		if (iCoopNetLandListNum >= 64) break;
		iCoopNetLandList[iCoopNetLandListNum] = iIdx;
		iCoopNetLandListNum++;
	}
	for (int k = 0; k < iCoopNetLandListNum; k++)
	{
		rChr = &Characters[iCoopNetLandList[k]];
		rChr.coopLandHidden.loc = rChr.location;
		rChr.coopLandHidden.group = rChr.location.group;
		rChr.coopLandHidden.locator = rChr.location.locator;
		ChangeCharacterAddressGroup(rChr, "none", "", "");
		rChr.location = "none";
		string sC = "c" + iCoopNetLandList[k];
		CoopNetLandHidden.(sC) = iCoopNetLandList[k];
	}
	if (iCoopNetLandListNum > 0) CoopNet_Log("land: " + iCoopNetLandListNum + " of our people stepped aside");
}

// Our hidden people come back: into the location at once when we are in it (bLogin), otherwise
// they only get their address back (the location's own people are made anew when it loads).
void CoopNet_LandRestoreHidden(bool bLogin)
{
	aref arAll;
	makearef(arAll, CoopNetLandHidden);
	int iNum = GetAttributesNum(arAll);
	for (int i = 0; i < iNum; i++)
	{
		aref arH = GetAttributeN(arAll, i);
		int iIdx = sti(GetAttributeValue(arH));
		ref rChr = &Characters[iIdx];
		if (!CheckAttribute(rChr, "coopLandHidden.loc")) continue;
		string sLoc = rChr.coopLandHidden.loc;
		string sGroup = rChr.coopLandHidden.group;
		string sLocator = rChr.coopLandHidden.locator;
		DeleteAttribute(rChr, "coopLandHidden");
		bool bFantom = iIdx >= LOC_FANTOM_CHARACTERS && iIdx < LOC_FANTOM_CHARACTERS + MAX_CHARS_IN_LOC;
		if (bLogin && sLoc == CoopNet_LandHere())
		{
			ChangeCharacterAddressGroup(rChr, sLoc, sGroup, sLocator);
			continue;
		}
		if (!bFantom)
		{
			rChr.location = sLoc;
			rChr.location.group = sGroup;
			rChr.location.locator = sLocator;
		}
	}
	if (iNum > 0) CoopNet_Log("land: " + iNum + " of our people back");
	DeleteAttribute(&CoopNetLandHidden, "");
}

// ---- other players' people ------------------------------------------------------------------

void CoopNet_OnLandPeople(int iFrom)
{
	int iId = CoopNet_GetInt();
	string sLoc = CoopNet_GetString();
	int iNum = CoopNet_GetInt();
	bool bRelay = false;
	if (CoopNet_State() == COOPNET_HOSTING && iId == iFrom && iId != CoopNet_MyId())
	{
		bRelay = true;
		CoopNet_Begin(COOPNET_MSG_LAND_PEOPLE);
		CoopNet_PutInt(iId);
		CoopNet_PutString(sLoc);
		CoopNet_PutInt(iNum);
	}
	bool bKeep = iId != CoopNet_MyId();
	if (CoopNet_State() == COOPNET_HOSTING && iId != iFrom) bKeep = false; // clients speak only for themselves
	for (int i = 0; i < iNum; i++)
	{
		int iIdx = CoopNet_GetInt();
		string sModel = CoopNet_GetString();
		string sAni = CoopNet_GetString();
		string sSex = CoopNet_GetString();
		float fX = CoopNet_GetFloat();
		float fY = CoopNet_GetFloat();
		float fZ = CoopNet_GetFloat();
		float fAY = CoopNet_GetFloat();
		float fVX = CoopNet_GetFloat();
		float fVZ = CoopNet_GetFloat();
		int iFlags = CoopNet_GetInt();
		if (bRelay)
		{
			CoopNet_PutInt(iIdx);
			CoopNet_PutString(sModel);
			CoopNet_PutString(sAni);
			CoopNet_PutString(sSex);
			CoopNet_PutFloat(fX);
			CoopNet_PutFloat(fY);
			CoopNet_PutFloat(fZ);
			CoopNet_PutFloat(fAY);
			CoopNet_PutFloat(fVX);
			CoopNet_PutFloat(fVZ);
			CoopNet_PutInt(iFlags);
		}
		if (!bKeep) continue;
		string sKey = "o" + iId + "_" + iIdx;
		CoopNetLandPeople.(sKey).owner = iId;
		CoopNetLandPeople.(sKey).idx = iIdx;
		CoopNetLandPeople.(sKey).loc = sLoc;
		aref arE;
		makearef(arE, CoopNetLandPeople.(sKey));
		CoopNet_LandStore(arE, fX, fY, fZ, fAY, fVX, fVZ, iFlags, sModel, sAni, sSex);
	}
	if (bRelay) CoopNet_Send(COOPNET_ALL, COOPNET_UNRELIABLE);
}

// ---- puppets ----------------------------------------------------------------------------

void CoopNet_LandUpdatePuppets(string sHere)
{
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
		CoopNet_LandKeepPuppet(arP, "CoopLand_" + arP.id, arP.name);
	}

	makearef(arAll, CoopNetLandPeople);
	iNum = GetAttributesNum(arAll);
	for (i = iNum - 1; i >= 0; i--)
	{
		arP = GetAttributeN(arAll, i);
		bStale = fNow - stf(arP.at) > COOPNET_LAND_PEOPLE_STALE;
		if (bStale || sHere == "" || arP.loc != sHere)
		{
			CoopNet_LandRemovePuppet(arP);
			DeleteAttribute(arAll, GetAttributeName(arP));
			continue;
		}
		CoopNet_LandKeepPuppet(arP, "CoopLand_" + arP.owner + "_" + arP.idx, "");
	}
}

void CoopNet_LandKeepPuppet(aref arP, string sId, string sName)
{
	if (CheckAttribute(arP, "chr"))
	{
		if (arP.chrModel != arP.model) CoopNet_LandRemovePuppet(arP); // changed clothes: made anew
	}
	if (!CheckAttribute(arP, "chr"))
	{
		if (!CoopNet_LandCreatePuppet(arP, sId, sName)) return;
	}
	CoopNet_LandSteer(arP);
}

bool CoopNet_LandCreatePuppet(aref arP, string sId, string sName)
{
	int iChr = NPC_GenerateCharacter(sId, arP.model, arP.sex, arP.ani, 1, PIRATE, -1, false);
	if (iChr < 0)
	{
		CoopNet_Log("land: no free character for " + sId);
		return false;
	}
	ref rChr = &Characters[iChr];
	string sC = "c" + iChr;
	DeleteAttribute(&CoopNetLandFree, sC);
	rChr.name = sName;
	rChr.lastname = "";
	rChr.coopLand = sId;
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
	arP.dead = false;
	if (sName != "") CoopNet_Log("land: " + sName + " as " + sId + " (" + arP.model + ") at " + arP.x + " " + arP.z);
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
	string sC = "c" + iChr;
	CoopNetLandFree.(sC) = iChr;
}

// Puppet characters go back to the free pool once nothing shows them any more.
void CoopNet_LandFreeSlots()
{
	aref arAll;
	makearef(arAll, CoopNetLandFree);
	int iNum = GetAttributesNum(arAll);
	for (int i = iNum - 1; i >= 0; i--)
	{
		aref arF = GetAttributeN(arAll, i);
		int iChr = sti(GetAttributeValue(arF));
		DeleteAttribute(arAll, GetAttributeName(arF));
		if (IsEntity(&Characters[iChr])) continue; // still logged in somewhere: kept as it is
		if (!CheckAttribute(&Characters[iChr], "coopLand")) continue;
		InitCharacter(&Characters[iChr], iChr);
	}
}

// This player's location goes away: every puppet leaves with it (before it can be saved with it),
// and our hidden people get their addresses back.
void CoopNet_LandForgetScene()
{
	CoopNet_LandForgetPuppets(&CoopNetLand);
	CoopNet_LandForgetPuppets(&CoopNetLandPeople);
	DeleteAttribute(&CoopNetLandPeople, "");
	CoopNet_LandRestoreHidden(false);
	bCoopNetLandOwner = true;
	sCoopNetLandArrivedLoc = "";
}

void CoopNet_LandForgetPuppets(ref rList)
{
	aref arAll;
	makearef(arAll, rList);
	int iNum = GetAttributesNum(arAll);
	for (int i = 0; i < iNum; i++)
	{
		aref arP = GetAttributeN(arAll, i);
		if (!CheckAttribute(arP, "chr")) continue;
		int iChr = sti(arP.chr);
		DeleteAttribute(arP, "chr");
		Characters[iChr].location = "none";
		string sC = "c" + iChr;
		CoopNetLandFree.(sC) = iChr;
	}
}

void CoopNet_LandSteer(aref arP)
{
	ref rChr = &Characters[sti(arP.chr)];
	int iFlags = sti(arP.flags);
	if (sti(arP.dead)) return;
	if (iFlags >= COOPNET_LAND_DEAD)
	{
		arP.dead = true;
		LAi_KillCharacter(rChr); // a scripted death: nobody gets the kill
		return;
	}
	bool bFight = iFlags == COOPNET_LAND_FIGHT;
	if (bFight != sti(arP.fight))
	{
		arP.fight = bFight;
		LAi_SetFightMode(rChr, bFight);
	}

	float fAge = CoopNet_Seconds() - stf(arP.at);
	float fVX = stf(arP.vx);
	float fVZ = stf(arP.vz);
	float fSpeed = sqrt(fVX * fVX + fVZ * fVZ);
	float fGX = stf(arP.x) + fVX * (fAge + COOPNET_LAND_LEAD);
	float fGZ = stf(arP.z) + fVZ * (fAge + COOPNET_LAND_LEAD);
	float fY = stf(arP.y);

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
}

// Every 10 s while a puppet is out: how far behind its player it walks, and how many people.
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
	makearef(arAll, CoopNetLandPeople);
	iNum = GetAttributesNum(arAll);
	int iShown = 0;
	int iDead = 0;
	float fWorst = 0.0;
	for (i = 0; i < iNum; i++)
	{
		arP = GetAttributeN(arAll, i);
		if (!CheckAttribute(arP, "chr")) continue;
		iShown++;
		if (sti(arP.dead))
		{
			iDead++;
			continue;
		}
		SendMessage(&Characters[sti(arP.chr)], "leee", MSG_CHARACTER_GETPOS, &fX, &fY, &fZ);
		fDX = stf(arP.x) - fX;
		fDZ = stf(arP.z) - fZ;
		float fOff = sqrt(fDX * fDX + fDZ * fDZ);
		if (fOff > fWorst) fWorst = fOff;
	}
	aref arHidden;
	makearef(arHidden, CoopNetLandHidden);
	fCoopNetLandLogAt = CoopNet_Seconds() + 10.0;
	CoopNet_Log("land: me in " + CoopNet_LandHere() + ", owner " + bCoopNetLandOwner + ", " + GetAttributesNum(arHidden) + " of ours aside, " + iShown + " of theirs shown (" + iDead + " dead, worst " + fWorst + " off)" + sLine);
}

// Returns true when the message was a land message.
bool CoopNet_LandDispatch(int iType, int iFrom)
{
	if (iType == COOPNET_MSG_LAND)
	{
		CoopNet_OnLand(iFrom);
		return true;
	}
	if (iType == COOPNET_MSG_LAND_PEOPLE)
	{
		CoopNet_OnLandPeople(iFrom);
		return true;
	}
	return false;
}

// ---- test -----------------------------------------------------------------------------------

int iCoopNetLandTestStep = 0;
int iCoopNetLandTestRoomTicks = 0;

// COOPNET_LANDTEST=1: both go from the hero room to Bridgetown, the client 15 s after the host (so
// the host owns the town). 10 s after reaching land the client's hero runs 10 m (moved 1 m every
// report, ~4 m/s), stands, runs back, stands, then draws its blade. 50 s after reaching the town the host goes to the
// tavern.
void CoopNet_TestLand()
{
	if (iCoopNetLandTestStep < 0 || CoopNet_GetEnv("COOPNET_LANDTEST") != "1") return;
	bool bClient = CoopNet_GetEnv("COOPNET_TEST") == "client";
	if (bCoopNetInLocation && !bSeaActive && CheckAttribute(pchar, "location") && iCoopNetLandTicks == 0)
	{
		if (pchar.location == "SelectMainCharacter_Cabine" && bCoopNetWorldInGame)
		{
			iCoopNetLandTestRoomTicks++;
			int iWait = 20;
			if (bClient) iWait = 80;
			if (iCoopNetLandTestRoomTicks == iWait) DoReloadCharacterToLocation("Bridgetown_town", "reload", "reload1");
		}
	}
	if (!bClient)
	{
		// the host leaves the town after a while: the client's own people come back
		if (CoopNet_LandHere() == "Bridgetown_town" && iCoopNetLandTicks == 0 && CoopNet_Seconds() - fCoopNetLandArrivedAt > 50.0)
		{
			CoopNet_Log("test: host goes to the tavern");
			iCoopNetLandTestStep = -1;
			DoReloadCharacterToLocation("Bridgetown_tavern", "reload", "reload1");
		}
		return;
	}
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
