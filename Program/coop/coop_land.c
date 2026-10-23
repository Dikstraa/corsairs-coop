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
//
// Fights: a puppet has a big reserve of health and is immortal. A hit on it is worked out here as
// usual (the attacker's weapon, skills, perks, experience), the health it lost is put back and sent
// to its owner, whose game takes it off the real character (the hero itself for a player's puppet)
// and makes it turn on the attacker's puppet there. Sides: the people an owner sends come as foes
// (hostile to its hero), allies (its officers) or neutral; a player's puppet is an ally, or a foe
// when the two flags are at war (PvP). Puppets are put in groups to match, so the owner's bandits go
// for the other players' puppets and everybody's officers go for the foes. Hits by our own on allies
// don't count.

#define COOPNET_MSG_LAND		160		// int id, string location, float x, y, z, ay, vx, vz, int flags, string model, string ani, string sex, string name, float here, int nation, extra
#define COOPNET_MSG_LAND_PEOPLE	161		// int id, string location, int n, n x (int idx, string model, string ani, string sex, float x, y, z, ay, vx, vz, int flags, int side, extra)
										// extra: int rank, int nation, string city, string type (GetCharType), float hp, float hpMax
#define COOPNET_MSG_LAND_HIT	162		// int owner, int idx + 1 (0: the hero), float damage, int attacker (player)
#define COOPNET_MSG_LAND_KILL	163		// int to (the player who finished it), int owner, int idx + 1
#define COOPNET_LAND_FIGHT		1		// flags: the blade is out
#define COOPNET_LAND_DEAD		2		// flags: dead (with the blade: 3)
#define COOPNET_LAND_NEUTRAL	0		// sides
#define COOPNET_LAND_FOE		1
#define COOPNET_LAND_ALLY		2
#define COOPNET_LAND_RESERVE	100000.0	// a puppet's health: what a hit takes is put back
#define COOPNET_LAND_FOES		"CoopFoes"		// groups for puppets (allies go to LAI_GROUP_PLAYER)
#define COOPNET_LAND_PEOPLE		"CoopPeople"
#define COOPNET_LAND_TICKS		5		// report every 5 ticks = 0.25 s
#define COOPNET_LAND_PEOPLE_EVERY	2	// people every 2nd report = 0.5 s
#define COOPNET_LAND_PER_MSG	6		// people per message (an unreliable message must fit one UDP packet, ~1190 bytes)
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
object CoopNetLandScratch;	// a message's extra fields while they are passed on
int iCoopNetLandTicks = 0;
int iCoopNetLandPeopleTicks = 0;
float fCoopNetLandFromX = 0.0;	// our position at the last report, for our speed
float fCoopNetLandFromZ = 0.0;
float fCoopNetLandFromAt = -1.0;
string sCoopNetLandFromLoc = "";
string sCoopNetLandArrivedLoc = "";	// where we are and since when
float fCoopNetLandArrivedAt = 0.0;
bool bCoopNetLandOwner = true;
bool bCoopNetLandGroups = false;	// the puppet groups are set up in this location
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
	CoopNet_PutInt(sti(pchar.nation));
	CoopNet_LandPutExtra(pchar);
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
	int iNation = CoopNet_GetInt();
	CoopNet_LandGetExtra(&CoopNetLandScratch);
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
		CoopNet_PutInt(iNation);
		CoopNet_LandPutExtraFrom(&CoopNetLandScratch);
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
	// a player is an ally, or a foe when our flags are at war
	int iSide = COOPNET_LAND_ALLY;
	if (iNation != sti(pchar.nation) && GetNationRelation(iNation, sti(pchar.nation)) == RELATION_ENEMY) iSide = COOPNET_LAND_FOE;
	CoopNetLand.(sKey).side = iSide;
	aref arP;
	makearef(arP, CoopNetLand.(sKey));
	CoopNet_LandStore(arP, fX, fY, fZ, fAY, fVX, fVZ, iFlags, sModel, sAni, sSex);
	CoopNet_LandCopyExtra(arP, &CoopNetLandScratch);
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
			int iSide = COOPNET_LAND_NEUTRAL;
			if (CoopNet_LandIsOfficer(rChr)) iSide = COOPNET_LAND_ALLY;
			else
			{
				if (LAi_group_IsEnemy(rChr, pchar)) iSide = COOPNET_LAND_FOE;
			}
			CoopNet_PutInt(iSide);
			CoopNet_LandPutExtra(rChr);
		}
		int iOk = 0;
		if (CoopNet_State() == COOPNET_HOSTING) iOk = CoopNet_Send(COOPNET_ALL, COOPNET_UNRELIABLE);
		else iOk = CoopNet_Send(COOPNET_HOST_ID, COOPNET_UNRELIABLE);
		if (iOk == 0) CoopNet_Log("land: people message not sent (" + iCount + " people)");
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
		int iSide = CoopNet_GetInt();
		CoopNet_LandGetExtra(&CoopNetLandScratch);
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
			CoopNet_PutInt(iSide);
			CoopNet_LandPutExtraFrom(&CoopNetLandScratch);
		}
		if (!bKeep) continue;
		string sKey = "o" + iId + "_" + iIdx;
		CoopNetLandPeople.(sKey).owner = iId;
		CoopNetLandPeople.(sKey).idx = iIdx;
		CoopNetLandPeople.(sKey).loc = sLoc;
		CoopNetLandPeople.(sKey).side = iSide;
		aref arE;
		makearef(arE, CoopNetLandPeople.(sKey));
		CoopNet_LandStore(arE, fX, fY, fZ, fAY, fVX, fVZ, iFlags, sModel, sAni, sSex);
		CoopNet_LandCopyExtra(arE, &CoopNetLandScratch);
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
		CoopNet_LandKeepPuppet(arP, "CoopLand_" + arP.id, arP.name, sti(arP.id), -1);
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
		CoopNet_LandKeepPuppet(arP, "CoopLand_" + arP.owner + "_" + arP.idx, "", sti(arP.owner), sti(arP.idx));
	}
}

void CoopNet_LandKeepPuppet(aref arP, string sId, string sName, int iOwner, int iIdx)
{
	if (CheckAttribute(arP, "chr"))
	{
		if (arP.chrModel != arP.model) CoopNet_LandRemovePuppet(arP); // changed clothes: made anew
	}
	if (!CheckAttribute(arP, "chr"))
	{
		if (!CoopNet_LandCreatePuppet(arP, sId, sName, iOwner, iIdx)) return;
	}
	CoopNet_LandSteer(arP);
}

bool CoopNet_LandCreatePuppet(aref arP, string sId, string sName, int iOwner, int iIdx)
{
	// the real one's rank (skills by rank: how well it defends) and nation, its town (what killing it
	// costs) and type (the kill statistics)
	int iChr = NPC_GenerateCharacter(sId, arP.model, arP.sex, arP.ani, sti(arP.rank), sti(arP.nation), -1, false);
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
	DeleteAttribute(rChr, "City");
	if (arP.city != "") rChr.City = arP.city;
	rChr.coopType = arP.ctype;
	rChr.coopLand = sId;
	rChr.coopOwner = iOwner;	// whose game has the real one, and its index there (-1: the hero)
	rChr.coopIdx = iIdx;
	rChr.coopSide = COOPNET_LAND_NEUTRAL;
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
	CoopNet_LandShowHealth(arP, rChr);
	UnsetSpecialAttributes(rChr); // the fight code reads chr_ai.special, made when a blade is equipped
	LAi_CharacterDisableDialog(rChr);
	TeleportCharacterToPosAy(rChr, stf(arP.x), stf(arP.y), stf(arP.z), stf(arP.ay));
	arP.chr = iChr;
	arP.chrModel = arP.model;
	arP.fight = false;
	arP.dead = false;
	arP.chrSide = -1;
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
	bCoopNetLandGroups = false;
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
	if (sti(arP.side) != sti(arP.chrSide)) CoopNet_LandSetSide(arP, rChr);
	CoopNet_LandShowHealth(arP, rChr);
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

// ---- extra fields: what a puppet needs to fight like the real one --------------------------------

void CoopNet_LandPutExtra(ref rChr)
{
	int iRank = 1;
	if (CheckAttribute(rChr, "rank")) iRank = sti(rChr.rank);
	string sCity = "";
	if (CheckAttribute(rChr, "City")) sCity = rChr.City;
	CoopNet_PutInt(iRank);
	CoopNet_PutInt(sti(rChr.nation));
	CoopNet_PutString(sCity);
	CoopNet_PutString(GetCharType(rChr));
	CoopNet_PutFloat(LAi_GetCharacterHP(rChr));
	CoopNet_PutFloat(LAi_GetCharacterMaxHP(rChr));
}

void CoopNet_LandGetExtra(aref arTo)
{
	arTo.rank = CoopNet_GetInt();
	arTo.nation = CoopNet_GetInt();
	arTo.city = CoopNet_GetString();
	arTo.ctype = CoopNet_GetString();
	arTo.hp = CoopNet_GetFloat();
	arTo.hpMax = CoopNet_GetFloat();
}

void CoopNet_LandPutExtraFrom(aref arFrom)
{
	CoopNet_PutInt(sti(arFrom.rank));
	CoopNet_PutInt(sti(arFrom.nation));
	CoopNet_PutString(arFrom.city);
	CoopNet_PutString(arFrom.ctype);
	CoopNet_PutFloat(stf(arFrom.hp));
	CoopNet_PutFloat(stf(arFrom.hpMax));
}

void CoopNet_LandCopyExtra(aref arTo, aref arFrom)
{
	arTo.rank = arFrom.rank;
	arTo.nation = arFrom.nation;
	arTo.city = arFrom.city;
	arTo.ctype = arFrom.ctype;
	arTo.hp = arFrom.hp;
	arTo.hpMax = arFrom.hpMax;
}

// ---- kills ----------------------------------------------------------------------------------------

// The owner says our hit finished one of its people: we get the kill, as if it were ours.
void CoopNet_OnLandKill(int iFrom)
{
	int iTo = CoopNet_GetInt();
	int iOwner = CoopNet_GetInt();
	int iIdx = CoopNet_GetInt() - 1;
	if (iTo != CoopNet_MyId())
	{
		if (CoopNet_State() != COOPNET_HOSTING) return;
		CoopNet_Begin(COOPNET_MSG_LAND_KILL);
		CoopNet_PutInt(iTo);
		CoopNet_PutInt(iOwner);
		CoopNet_PutInt(iIdx + 1);
		CoopNet_Send(iTo, COOPNET_RELIABLE);
		return;
	}
	string sKey = "o" + iOwner + "_" + iIdx;
	if (!CheckAttribute(&CoopNetLandPeople, sKey + ".chr")) return;
	ref rDead;
	makeref(rDead, Characters[sti(CoopNetLandPeople.(sKey).chr)]);
	int iBy = nMainCharacterIndex;
	if (CheckAttribute(rDead, "coopLastBy")) iBy = sti(rDead.coopLastBy);
	bool bGun = false;
	if (CheckAttribute(rDead, "coopLastGun")) bGun = sti(rDead.coopLastGun);
	ref rBy;
	makeref(rBy, Characters[iBy]);
	CoopNet_LandKillReward(rBy, rDead, bGun);
	CoopNet_Log("land: " + rBy.id + " gets the kill of " + rDead.id + " (" + rDead.coopType + ", gun " + bGun + ")");
}

// What LAi_ApplyCharacterAttackDamage / LAi_ApplyCharacterFireDamage give for a kill.
void CoopNet_LandKillReward(ref attack, ref enemy, bool bGun)
{
	float ra = 1.0;
	float re = 1.0;
	if (CheckAttribute(attack, "rank")) ra = stf(attack.rank);
	if (CheckAttribute(enemy, "rank")) re = stf(enemy.rank);
	int experience = 0;
	if (bGun)
	{
		if (re < 4) experience = GetCharacterSPECIALSimple(attack, SPECIAL_P) + rand(GetCharacterSPECIALSimple(attack, SPECIAL_I));
		else experience = LAi_GetCharacterMaxHP(enemy) / re + GetCharacterSPECIALSimple(attack, SPECIAL_P) * re / ra;
		AddCharacterExpToSkill(attack, SKILL_PISTOL, experience);
		AddCharacterExpToSkill(attack, SKILL_DEFENCE, 1);
		AddCharacterExpToSkill(attack, SKILL_FORTUNE, 2);
		AddCharacterExpToSkill(attack, SKILL_LEADERSHIP, 1);
	}
	else
	{
		if (re < 4) experience = GetCharacterSPECIALSimple(attack, SPECIAL_I) + rand(GetCharacterSPECIALSimple(attack, SPECIAL_A));
		else experience = LAi_GetCharacterMaxHP(enemy) / re + GetCharacterSPECIALSimple(attack, SPECIAL_I) * re / ra;
		AddCharacterExpToSkill(attack, LAi_GetBladeFencingType(attack), experience);
		AddCharacterExpToSkill(attack, SKILL_DEFENCE, 1);
		AddCharacterExpToSkill(attack, SKILL_FORTUNE, 1);
		AddCharacterExpToSkill(attack, SKILL_LEADERSHIP, 1);
	}
	// the statistics by the real one's type (a puppet is an actor, which counts as a citizen)
	if (sti(attack.index) == GetMainCharacterIndex())
	{
		string sStat = enemy.coopType + "_s";
		if (bGun) sStat = enemy.coopType + "_g";
		Statistic_AddValue(attack, sStat, 1);
	}
	LAi_SetResultOfDeath(attack, enemy, true);
}

// ---- fights ---------------------------------------------------------------------------------

// A puppet's group follows its side: foes are hostile to our hero's group, allies are in it.
void CoopNet_LandSetSide(aref arP, ref rChr)
{
	if (!bCoopNetLandGroups)
	{
		bCoopNetLandGroups = true;
		LAi_group_Register(COOPNET_LAND_FOES);
		LAi_group_Register(COOPNET_LAND_PEOPLE);
		LAi_group_SetRelation(COOPNET_LAND_FOES, LAI_GROUP_PLAYER, LAI_GROUP_ENEMY);
		LAi_group_SetRelation(COOPNET_LAND_PEOPLE, LAI_GROUP_PLAYER, LAI_GROUP_NEITRAL);
		LAi_group_SetRelation(COOPNET_LAND_FOES, COOPNET_LAND_PEOPLE, LAI_GROUP_NEITRAL);
	}
	int iSide = sti(arP.side);
	arP.chrSide = iSide;
	rChr.coopSide = iSide;
	string sGroup = COOPNET_LAND_PEOPLE;
	if (iSide == COOPNET_LAND_FOE) sGroup = COOPNET_LAND_FOES;
	if (iSide == COOPNET_LAND_ALLY) sGroup = LAI_GROUP_PLAYER;
	LAi_group_MoveCharacter(rChr, sGroup);
}

// A puppet shows the real one's health (and stays alive: it is immortal outside a hit).
void CoopNet_LandShowHealth(aref arP, ref rChr)
{
	float fMax = stf(arP.hpMax);
	if (fMax < 1.0) fMax = 1.0;
	float fHp = stf(arP.hp);
	if (fHp < 1.0) fHp = 1.0;
	if (CheckAttribute(rChr, "coopHitAt") && CheckAttribute(rChr, "coopHp"))
	{
		// the owner's word may still be from before our last hit
		if (CoopNet_Seconds() - stf(rChr.coopHitAt) < 1.0 && stf(rChr.coopHp) < fHp) fHp = stf(rChr.coopHp);
	}
	rChr.coopHp = fHp;
	rChr.coopHpMax = fMax;
	rChr.chr_ai.hp_max = fMax;
	rChr.chr_ai.hp = fHp;
}

// From LAi_CharacterAttack and LAi_CharacterFire (LAi_events.c) around the damage: -1 when chr is
// no puppet, else the reserve it gets for the hit, from which the hit is measured.
float CoopNet_LandHitBefore(aref chr)
{
	if (!CheckAttribute(chr, "coopOwner")) return -1.0;
	chr.chr_ai.immortal = "0"; // immortals take no hits at all; the reserve keeps it alive
	chr.chr_ai.hp_max = COOPNET_LAND_RESERVE;
	chr.chr_ai.hp = COOPNET_LAND_RESERVE;
	return COOPNET_LAND_RESERVE;
}

void CoopNet_LandHitAfter(aref attack, aref chr, float fBefore, string sKind)
{
	CoopNet_LandHitDone(attack, chr, fBefore, false, sKind);
}

void CoopNet_LandHitAfterGun(aref attack, aref chr, float fBefore)
{
	CoopNet_LandHitDone(attack, chr, fBefore, true, "gun");
}

void CoopNet_LandHitDone(aref attack, aref chr, float fBefore, bool bGun, string sKind)
{
	if (fBefore < 0.0) return;
	float fDamage = fBefore - stf(chr.chr_ai.hp);
	// back to what the real one has, less this hit until the owner's next word
	float fHp = stf(chr.coopHp) - fDamage;
	if (fHp < 1.0) fHp = 1.0;
	chr.coopHp = fHp;
	chr.coopHitAt = CoopNet_Seconds();
	chr.chr_ai.hp_max = chr.coopHpMax;
	chr.chr_ai.hp = fHp;
	chr.chr_ai.immortal = "1";
	if (!CheckAttribute(attack, "coopOwner"))
	{
		// who of ours hit it last, and with what: the one who gets the kill
		chr.coopLastBy = attack.index;
		chr.coopLastGun = bGun;
	}
	DeleteAttribute(chr, "chr_ai.Blooding");
	DeleteAttribute(chr, "chr_ai.poison");
	if (sti(chr.coopSide) == COOPNET_LAND_NEUTRAL)
	{
		// the game set the whole group on us; the owner's game decides who is hostile now
		LAi_group_SetRelation(COOPNET_LAND_PEOPLE, LAI_GROUP_PLAYER, LAI_GROUP_NEITRAL);
		LAi_group_SetAlarm(COOPNET_LAND_PEOPLE, LAI_GROUP_PLAYER, 0.0);
	}
	if (fDamage <= 0.0) return;
	if (CheckAttribute(attack, "coopOwner")) return; // a puppet's swing: its own game counts that
	if (sti(chr.coopSide) == COOPNET_LAND_ALLY)
	{
		// our own don't hurt allies (another player under a friendly flag, its officers)
		if (sti(attack.index) == nMainCharacterIndex || CoopNet_LandIsOfficer(attack)) return;
	}
	int iOwner = sti(chr.coopOwner);
	CoopNet_Begin(COOPNET_MSG_LAND_HIT);
	CoopNet_PutInt(iOwner);
	CoopNet_PutInt(sti(chr.coopIdx) + 1); // sent +1: a negative int from the wire compares as unsigned
	CoopNet_PutFloat(fDamage);
	CoopNet_PutInt(CoopNet_MyId());
	// who of ours dealt it (+1, 0: the hero) and how: its puppet over there swings the same way
	int iBy = sti(attack.index) + 1;
	if (sti(attack.index) == nMainCharacterIndex) iBy = 0;
	CoopNet_PutInt(iBy);
	CoopNet_PutString(sKind);
	if (CoopNet_State() == COOPNET_HOSTING) CoopNet_Send(iOwner, COOPNET_RELIABLE);
	else CoopNet_Send(COOPNET_HOST_ID, COOPNET_RELIABLE);
	CoopNet_Log("land: " + attack.id + " hit " + chr.id + " for " + fDamage);
}

// A hit on our hero's puppet in another player's game.
void CoopNet_LandHeroHit(float fDamage, int iAttacker)
{
	if (LAi_IsDead(pchar)) return;
	LAi_ApplyCharacterDamage(pchar, makeint(fDamage + 0.5));
	LAi_CheckKillCharacter(pchar);
	CoopNet_Log("land: our hero hit by player " + iAttacker + " for " + fDamage + ", hp " + pchar.chr_ai.hp);
}

// The owner takes a hit from another player's game off the real character, which turns on that
// player's puppet here.
void CoopNet_OnLandHit(int iFrom)
{
	int iOwner = CoopNet_GetInt();
	int iIdx = CoopNet_GetInt() - 1;
	float fDamage = CoopNet_GetFloat();
	int iAttacker = CoopNet_GetInt();
	int iByIdx = CoopNet_GetInt() - 1;
	string sKind = CoopNet_GetString();
	CoopNet_Log("land: hit for player " + iOwner + " character " + iIdx + " by player " + iAttacker + ", " + fDamage);
	if (iOwner != CoopNet_MyId())
	{
		if (CoopNet_State() != COOPNET_HOSTING) return;
		CoopNet_Begin(COOPNET_MSG_LAND_HIT);
		CoopNet_PutInt(iOwner);
		CoopNet_PutInt(iIdx + 1);
		CoopNet_PutFloat(fDamage);
		CoopNet_PutInt(iAttacker);
		CoopNet_PutInt(iByIdx + 1);
		CoopNet_PutString(sKind);
		CoopNet_Send(iOwner, COOPNET_RELIABLE);
		return;
	}
	string sHere = CoopNet_LandHere();
	if (sHere == "")
	{
		CoopNet_Log("land: hit dropped, we are not on land");
		return;
	}
	// the attacker's puppet here: the other player or one of its people; it swings at the target
	int iBy = CoopNet_LandFindPuppet(iAttacker, iByIdx);
	ref rTarget;
	makeref(rTarget, Characters[nMainCharacterIndex]);
	if (iIdx >= 0) makeref(rTarget, Characters[iIdx]);
	if (iBy >= 0) CoopNet_LandSwing(iBy, rTarget, sKind);
	if (iIdx < 0)
	{
		CoopNet_LandHeroHit(fDamage, iAttacker);
		return;
	}
	if (!IsEntity(rTarget) || !CheckAttribute(rTarget, "location")) return;
	if (rTarget.location != sHere) return;
	if (LAi_IsDead(rTarget))
	{
		CoopNet_Log("land: hit dropped, " + rTarget.id + " is dead already");
		return;
	}
	LAi_ApplyCharacterDamage(rTarget, makeint(fDamage + 0.5));
	LAi_CheckKillCharacter(rTarget);
	bool bDead = LAi_IsDead(rTarget);
	CoopNet_Log("land: " + rTarget.id + " hit by player " + iAttacker + " for " + fDamage + ", hp " + rTarget.chr_ai.hp + ", dead " + bDead);
	if (bDead)
	{
		// the player who finished it gets the kill
		CoopNet_Begin(COOPNET_MSG_LAND_KILL);
		CoopNet_PutInt(iAttacker);
		CoopNet_PutInt(CoopNet_MyId());
		CoopNet_PutInt(iIdx + 1);
		if (CoopNet_State() == COOPNET_HOSTING) CoopNet_Send(iAttacker, COOPNET_RELIABLE);
		else CoopNet_Send(COOPNET_HOST_ID, COOPNET_RELIABLE);
		return;
	}
	// it fights back: against whoever hit it (the puppet of that player or of its officer)
	if (iBy < 0) return;
	ref rBy;
	makeref(rBy, Characters[iBy]);
	LAi_group_Attack(rBy, rTarget);
	string sFunc = rTarget.chr_ai.type;
	if (sFunc == "") return;
	sFunc = "LAi_type_" + sFunc + "_Attacked";
	call sFunc(rTarget, rBy);
}

// The character index of the puppet of player iPlayer's hero (iIdx -1) or of its character iIdx;
// -1 when it isn't shown here.
int CoopNet_LandFindPuppet(int iPlayer, int iIdx)
{
	string sKey = "p" + iPlayer;
	if (iIdx < 0)
	{
		if (!CheckAttribute(&CoopNetLand, sKey + ".chr")) return -1;
		return sti(CoopNetLand.(sKey).chr);
	}
	sKey = "o" + iPlayer + "_" + iIdx;
	if (!CheckAttribute(&CoopNetLandPeople, sKey + ".chr")) return -1;
	return sti(CoopNetLandPeople.(sKey).chr);
}

// A puppet turns to its target and swings as its real one did (the blow itself came by message).
void CoopNet_LandSwing(int iBy, ref rTarget, string sKind)
{
	ref rBy;
	makeref(rBy, Characters[iBy]);
	if (!IsEntity(rBy) || LAi_IsDead(rBy)) return;
	CharacterTurnByChr(rBy, rTarget);
	string sAction = "attack_fast_" + (1 + rand(2));
	if (sKind == "force") sAction = "attack_force_" + (1 + rand(3));
	if (sKind == "round") sAction = "attack_round_1";
	if (sKind == "break") sAction = "attack_break_1";
	if (sKind == "gun") sAction = "Shot";
	CharacterPlayAction(rBy, sAction);
	CoopNet_Log("land: " + rBy.id + " swings (" + sAction + ") at " + rTarget.id);
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
	if (iType == COOPNET_MSG_LAND_HIT)
	{
		CoopNet_OnLandHit(iFrom);
		return true;
	}
	if (iType == COOPNET_MSG_LAND_KILL)
	{
		CoopNet_OnLandKill(iFrom);
		return true;
	}
	return false;
}

// ---- test -----------------------------------------------------------------------------------

int iCoopNetLandTestStep = 0;
int iCoopNetLandTestRoomTicks = 0;

// COOPNET_LANDTEST=1: both go from the hero room to Bridgetown, the client 15 s after the host (so
// the host owns the town). 10 s after reaching land the client's hero runs 10 m (moved 1 m every
// report, ~4 m/s), stands, runs back, stands, then draws its blade and strikes one of the host's
// people 5 times. 35 s after reaching the town one of the host's people strikes the client's
// puppet once; after 75 s the host goes to the tavern.
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
		if (CoopNet_LandHere() == "Bridgetown_town" && iCoopNetLandTicks == 0 && iCoopNetLandTestStep == 0 && CoopNet_Seconds() - fCoopNetLandArrivedAt > 35.0)
		{
			iCoopNetLandTestStep = 1;
			CoopNet_TestLandStrikeBack();
		}
		if (CoopNet_LandHere() == "Bridgetown_town" && iCoopNetLandTicks == 0 && iCoopNetLandTestStep == 1 && CoopNet_Seconds() - fCoopNetLandArrivedAt > 45.0)
		{
			iCoopNetLandTestStep = 2;
			CoopNet_TestLandStrikeOfficer();
		}
		// the host leaves the town after a while: the client's own people come back
		if (CoopNet_LandHere() == "Bridgetown_town" && iCoopNetLandTicks == 0 && CoopNet_Seconds() - fCoopNetLandArrivedAt > 75.0)
		{
			CoopNet_Log("test: host goes to the tavern");
			iCoopNetLandTestStep = -1;
			DoReloadCharacterToLocation("Bridgetown_tavern", "reload", "reload1");
		}
		return;
	}
	if (CoopNet_LandHere() == "" || iCoopNetLandTicks != 0) return;
	iCoopNetLandTestStep++;
	if (iCoopNetLandTestStep == 1) CoopNet_TestLandOfficer();
	int iStep = iCoopNetLandTestStep - 40;
	if (iStep < 0) return;
	float fX = 0.0;
	float fY = 0.0;
	float fZ = 0.0;
	SendMessage(pchar, "leee", MSG_CHARACTER_GETPOS, &fX, &fY, &fZ);
	if (iStep == 0) CoopNet_Log("test: land run from " + fX + " " + fZ);
	if (iStep < 10) TeleportCharacterToPos(pchar, fX + 1.0, fY, fZ);
	if (iStep >= 30 && iStep < 40) TeleportCharacterToPos(pchar, fX - 1.0, fY, fZ);
	if (iStep == 50)
	{
		int iOff = GetCharacterIndex("CoopTestOfficer");
		if (iOff >= 0) CoopNet_TestLandStrike(&Characters[iOff]);
	}
	if (iStep >= 65 && iStep < 70) CoopNet_TestLandStrike(pchar);
	if (iStep == 30 || iStep == 72) CoopNet_Log("test: client hp " + pchar.chr_ai.hp);
	if (iStep == 60)
	{
		CoopNet_Log("test: land blade out");
		LAi_SetFightMode(pchar, true);
	}
	if (iStep == 80) iCoopNetLandTestStep = -1;
}

// COOPNET_LANDTEST: blows as LAi_CharacterAttack deals them, wrapped like there.
void CoopNet_TestLandBlow(ref rBy, ref rOn)
{
	float fBefore = CoopNet_LandHitBefore(rOn);
	LAi_ApplyCharacterAttackDamage(rBy, rOn, "break", false, false);
	CoopNet_LandHitAfter(rBy, rOn, fBefore, "break");
}

// The client strikes the first of the host's people it shows.
void CoopNet_TestLandStrike(ref rBy)
{
	aref arAll;
	makearef(arAll, CoopNetLandPeople);
	int iNum = GetAttributesNum(arAll);
	for (int i = 0; i < iNum; i++)
	{
		aref arE = GetAttributeN(arAll, i);
		if (!CheckAttribute(arE, "chr") || sti(arE.dead)) continue;
		ref rOn;
		makeref(rOn, Characters[sti(arE.chr)]);
		CoopNet_Log("test: " + rBy.id + " strikes");
		CoopNet_Log("test: land strike at " + rOn.id + " (" + arE.owner + "/" + arE.idx + "), rank " + rOn.rank + ", hp " + rOn.chr_ai.hp + "/" + rOn.chr_ai.hp_max + ", type " + rOn.coopType);
		CoopNet_TestLandBlow(rBy, rOn);
		return;
	}
	CoopNet_Log("test: land strike - nobody to strike");
}

// The host: one of its people strikes the client's puppet.
void CoopNet_TestLandStrikeBack()
{
	if (!CheckAttribute(&CoopNetLand, "p1.chr")) return;
	ref rOn;
	makeref(rOn, Characters[sti(CoopNetLand.p1.chr)]);
	for (int i = 0; i < LAi_numloginedcharacters; i++)
	{
		int iIdx = LAi_loginedcharacters[i];
		if (iIdx < GlobalCharacters || iIdx == nMainCharacterIndex) continue;
		if (CheckAttribute(&Characters[iIdx], "coopLand")) continue;
		ref rBy;
		makeref(rBy, Characters[iIdx]);
		CoopNet_Log("test: " + rBy.id + " strikes the client's puppet");
		CoopNet_TestLandBlow(rBy, rOn);
		return;
	}
}

// The client gets an officer for the test (a made-up one, standing by the hero).
void CoopNet_TestLandOfficer()
{
	int iOff = NPC_GenerateCharacter("CoopTestOfficer", "officer_15", "man", "man", 10, ENGLAND, -1, true);
	if (iOff < 0) return;
	ref rOff;
	makeref(rOff, Characters[iOff]);
	ChangeCharacterAddressGroup(rOff, pchar.location, pchar.location.group, pchar.location.locator);
	LAi_SetOfficerType(rOff);
	CoopNet_Log("test: officer " + rOff.id + " (" + iOff + ") by the hero");
}

// The host: one of its people strikes the client's officer's puppet.
void CoopNet_TestLandStrikeOfficer()
{
	aref arAll;
	makearef(arAll, CoopNetLandPeople);
	int iNum = GetAttributesNum(arAll);
	for (int i = 0; i < iNum; i++)
	{
		aref arE = GetAttributeN(arAll, i);
		if (sti(arE.owner) != 1 || !CheckAttribute(arE, "chr")) continue;
		ref rOn;
		makeref(rOn, Characters[sti(arE.chr)]);
		for (int k = 0; k < LAi_numloginedcharacters; k++)
		{
			int iIdx = LAi_loginedcharacters[k];
			if (iIdx < GlobalCharacters || iIdx == nMainCharacterIndex) continue;
			if (CheckAttribute(&Characters[iIdx], "coopLand") || LAi_IsDead(&Characters[iIdx])) continue;
			ref rBy;
			makeref(rBy, Characters[iIdx]);
			CoopNet_Log("test: " + rBy.id + " strikes the client's officer's puppet " + rOn.id);
			CoopNet_TestLandBlow(rBy, rOn);
			return;
		}
	}
	CoopNet_Log("test: no officer puppet of the client here");
}
