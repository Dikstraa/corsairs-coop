// CoopNet sea: players at sea in the same place see each other's ships.
//
// Every player at sea sends its ship four times a second: the island (or "" in open sea), the
// position, heading, speed and ship type. The host passes it on to everybody. Positions near an
// island are in that island's sea coordinates, which are the same for everybody there; in open sea
// each player's coordinates start where it left the world map, so they are sent as world map
// coordinates (sea / scale + the map point the sea scene was made around) and turned back into
// the receiver's own sea coordinates.
// Another player within COOPNET_SEA_RANGE is shown as a "puppet": a friendly NPC with a ship of the
// same type, sailing (AI move task) to where that player's ship will be a few seconds later, its speed
// matched to close the gap. A ship can't be moved or removed in a running sea scene, so a puppet whose
// player leaves sails away until it comes back or the scene ends.

#define COOPNET_MSG_SEA			130		// int id, string island, float x, z, ay, speed, int baseType, string shipName
#define COOPNET_SEA_TICKS		5		// report every 5 ticks = 0.25 s
#define COOPNET_SEA_STEER_TICKS	20		// new move task for puppets every second
#define COOPNET_SEA_RANGE		2500.0	// sea units: farther players are not in this scene
#define COOPNET_SEA_LEAD		6.0		// seconds ahead a puppet sails to
#define COOPNET_SEA_CATCH_UP	0.15	// extra speed per sea unit a puppet is behind
#define COOPNET_SEA_STALE		3.0		// seconds without a report before a player has left the sea
#define COOPNET_SEA_PARK		10000.0	// how far a puppet sails off when its player leaves

object CoopNetSea;	// p<id>: id, island, x, z, ay, speed, type, name, at; puppet: chr, inSea, parked
int iCoopNetSeaTicks = 0;
int iCoopNetSeaSteerTicks = 0;
int iCoopNetSeaLogTicks = 0;

void CoopNet_SeaTick()
{
	int iState = CoopNet_State();
	if (iState != COOPNET_CONNECTED && iState != COOPNET_HOSTING) return;
	bool bAtSea = false;
	if (bSeaActive && iCoopNetMode == COOPNET_MODE_SEA)
	{
		if (CheckAttribute(pchar, "Ship.Pos.x")) bAtSea = true;
	}
	if (!bSeaActive) CoopNet_SeaForgetScene();

	iCoopNetSeaTicks++;
	if (iCoopNetSeaTicks >= COOPNET_SEA_TICKS)
	{
		iCoopNetSeaTicks = 0;
		if (bAtSea) CoopNet_SeaReport();
	}
	if (!bAtSea) return;
	iCoopNetSeaSteerTicks++;
	if (iCoopNetSeaSteerTicks < COOPNET_SEA_STEER_TICKS) return;
	iCoopNetSeaSteerTicks = 0;
	CoopNet_SeaUpdatePuppets();
	CoopNet_SeaLog();
}

// Sea units per world map unit around this player's scene (as WdmAddPlayerGroup computes it).
float CoopNet_SeaScale()
{
	if (worldMap.island == WDM_NONE_ISLAND) return makefloat(WDM_MAP_TO_SEA_SCALE);
	string sIsland = worldMap.island;
	if (sIsland == "Cuba1" || sIsland == "Cuba2" || sIsland == "Beliz" || sIsland == "SantaCatalina" || sIsland == "PortoBello" || sIsland == "Cartahena" || sIsland == "Maracaibo" || sIsland == "Caracas" || sIsland == "Cumana") return 25.0;
	return makefloat(WDM_MAP_TO_SEA_SCALE);
}

void CoopNet_SeaReport()
{
	string sIsland = pchar.location;
	float fX = stf(pchar.Ship.Pos.x);
	float fZ = stf(pchar.Ship.Pos.z);
	if (sIsland == "")
	{
		float fScale = CoopNet_SeaScale();
		fX = fX / fScale + stf(worldMap.zeroX);
		fZ = fZ / fScale + stf(worldMap.zeroZ);
	}
	float fSpeed = 0.0;
	if (CheckAttribute(pchar, "Ship.Speed.z")) fSpeed = stf(pchar.Ship.Speed.z);
	int iType = sti(RealShips[sti(pchar.Ship.Type)].BaseType);
	CoopNet_Begin(COOPNET_MSG_SEA);
	CoopNet_PutInt(CoopNet_MyId());
	CoopNet_PutString(sIsland);
	CoopNet_PutFloat(fX);
	CoopNet_PutFloat(fZ);
	CoopNet_PutFloat(stf(pchar.Ship.Ang.y));
	CoopNet_PutFloat(fSpeed);
	CoopNet_PutInt(iType);
	CoopNet_PutString(pchar.Ship.Name);
	if (CoopNet_State() == COOPNET_HOSTING) CoopNet_Send(COOPNET_ALL, COOPNET_UNRELIABLE);
	else CoopNet_Send(COOPNET_HOST_ID, COOPNET_UNRELIABLE);
}

void CoopNet_OnSea(int iFrom)
{
	int iId = CoopNet_GetInt();
	string sIsland = CoopNet_GetString();
	float fX = CoopNet_GetFloat();
	float fZ = CoopNet_GetFloat();
	float fAY = CoopNet_GetFloat();
	float fSpeed = CoopNet_GetFloat();
	int iType = CoopNet_GetInt();
	string sName = CoopNet_GetString();
	if (iId == CoopNet_MyId()) return;
	if (CoopNet_State() == COOPNET_HOSTING)
	{
		if (iId != iFrom) return; // clients speak only for themselves
		CoopNet_Begin(COOPNET_MSG_SEA);
		CoopNet_PutInt(iId);
		CoopNet_PutString(sIsland);
		CoopNet_PutFloat(fX);
		CoopNet_PutFloat(fZ);
		CoopNet_PutFloat(fAY);
		CoopNet_PutFloat(fSpeed);
		CoopNet_PutInt(iType);
		CoopNet_PutString(sName);
		CoopNet_Send(COOPNET_ALL, COOPNET_UNRELIABLE);
	}
	string sKey = "p" + iId;
	CoopNetSea.(sKey).id = iId;
	CoopNetSea.(sKey).island = sIsland;
	CoopNetSea.(sKey).x = fX;
	CoopNetSea.(sKey).z = fZ;
	CoopNetSea.(sKey).ay = fAY;
	CoopNetSea.(sKey).speed = fSpeed;
	CoopNetSea.(sKey).type = iType;
	CoopNetSea.(sKey).name = sName;
	CoopNetSea.(sKey).at = CoopNet_Seconds();
	if (!CheckAttribute(&CoopNetSea, sKey + ".inSea")) CoopNetSea.(sKey).inSea = false;
}

// The sea scene ended: its puppets went with it.
void CoopNet_SeaForgetScene()
{
	aref arList;
	makearef(arList, CoopNetSea);
	int iNum = GetAttributesNum(arList);
	for (int i = 0; i < iNum; i++)
	{
		aref arP = GetAttributeN(arList, i);
		arP.inSea = false;
	}
}

// Where another player's ship is in this scene (arP.lx, arP.lz); false when it isn't in this scene.
// (A result through ref float parameters is unreliable in this script engine: the callee's own
// locals end up in them.)
bool CoopNet_SeaLocalPos(aref arP)
{
	if (CoopNet_Seconds() - stf(arP.at) > COOPNET_SEA_STALE) return false;
	string sMine = pchar.location;
	if (arP.island != sMine) return false;
	float fX = stf(arP.x);
	float fZ = stf(arP.z);
	if (sMine == "")
	{
		float fScale = CoopNet_SeaScale();
		fX = (fX - stf(worldMap.zeroX)) * fScale;
		fZ = (fZ - stf(worldMap.zeroZ)) * fScale;
	}
	arP.lx = fX;
	arP.lz = fZ;
	float fDX = fX - stf(pchar.Ship.Pos.x);
	float fDZ = fZ - stf(pchar.Ship.Pos.z);
	return fDX * fDX + fDZ * fDZ < COOPNET_SEA_RANGE * COOPNET_SEA_RANGE;
}
void CoopNet_SeaUpdatePuppets()
{
	aref arList;
	makearef(arList, CoopNetSea);
	int iNum = GetAttributesNum(arList);
	for (int i = 0; i < iNum; i++)
	{
		aref arP = GetAttributeN(arList, i);
		if (!CoopNet_SeaLocalPos(arP))
		{
			if (sti(arP.inSea) && !sti(arP.parked)) CoopNet_SeaPark(arP);
			continue;
		}
		if (!sti(arP.inSea))
		{
			CoopNet_SeaCreatePuppet(arP, stf(arP.lx), stf(arP.lz));
			continue;
		}
		CoopNet_SeaSteer(arP, stf(arP.lx), stf(arP.lz));
	}
}

string CoopNet_SeaGroup(aref arP)
{
	return "CoopSea_" + arP.id;
}

void CoopNet_SeaCreatePuppet(aref arP, float fX, float fZ)
{
	string sId = "CoopPuppet_" + arP.id;
	int iChr = GetCharacterIndex(sId);
	if (iChr < 0) iChr = NPC_GenerateCharacter(sId, "officer_15", "man", "man", 10, sti(pchar.nation), -1, false);
	if (iChr < 0) return;
	ref rChr = &Characters[iChr];
	int iType = sti(arP.type);
	if (!CheckAttribute(rChr, "coopShipBase") || sti(rChr.coopShipBase) != iType)
	{
		rChr.Ship.Type = GenerateShipExt(iType, true, rChr);
		rChr.coopShipBase = iType;
		SetBaseShipData(rChr);
	}
	rChr.Ship.Name = arP.name;
	DeleteAttribute(rChr, "coopSpeed");
	rChr.AlwaysFriend = true;
	rChr.nation = pchar.nation;
	DeleteAttribute(rChr, "Ship.Sink");
	string sGroup = CoopNet_SeaGroup(arP);
	Group_FindOrCreateGroup(sGroup);
	Group_SetXZ_AY(sGroup, fX, fZ, stf(arP.ay));
	Group_SetType(sGroup, "war");
	Group_AddCharacter(sGroup, sId);
	Group_SetGroupCommander(sGroup, sId);
	rChr.SeaAI.Group.Name = sGroup;
	Ship_Add2Sea(iChr, false, "");
	SetCharacterRelationBoth(iChr, nMainCharacterIndex, RELATION_FRIEND);
	arP.chr = iChr;
	arP.inSea = true;
	arP.parked = false;
	CoopNet_Log("sea: player " + arP.id + " as " + sId + " (ship type " + iType + ") at " + fX + " " + fZ);
	CoopNet_SeaSteer(arP, fX, fZ);
}

// The puppet sails for a point ahead of the player's ship; its top speed is the player's speed plus
// a share of the distance it is behind (Ship_UpdateParameters takes it from coopSpeed), so it
// catches up and doesn't overshoot. (Ships in a running sea scene can't be moved or removed:
// neither Group_SetXZ_AY nor writing Ship.Pos moves them.)
void CoopNet_SeaSteer(aref arP, float fX, float fZ)
{
	int iChr = sti(arP.chr);
	ref rChr = &Characters[iChr];
	if (!CheckAttribute(rChr, "Ship.Pos.x")) return;
	float fAY = stf(arP.ay);
	float fSpeed = stf(arP.speed);
	// the player's ship kept sailing since its report
	float fAge = CoopNet_Seconds() - stf(arP.at);
	float fDirX = sin(fAY);
	float fDirZ = cos(fAY);
	fX = fX + fDirX * fSpeed * fAge;
	fZ = fZ + fDirZ * fSpeed * fAge;
	float fBehind = (fX - stf(rChr.Ship.Pos.x)) * fDirX + (fZ - stf(rChr.Ship.Pos.z)) * fDirZ;
	float fWanted = fSpeed + fBehind * COOPNET_SEA_CATCH_UP;
	float fMax = fSpeed * 2.5;
	if (fMax < 20.0) fMax = 20.0;
	if (fWanted > fMax) fWanted = fMax;
	if (fWanted < 0.0) fWanted = 0.0;
	rChr.coopSpeed = fWanted;
	rChr.Ship.MaxSpeedZ = fWanted;
	if (sti(arP.parked))
	{
		CoopNet_Log("sea: player " + arP.id + " is back in this scene");
		arP.parked = false;
	}
	float fLead = fSpeed * COOPNET_SEA_LEAD;
	if (fLead < 30.0) fLead = 30.0; // a goal right under the bow keeps the ship turning
	Ship_SetTaskMove(PRIMARY_TASK, iChr, fX + fDirX * fLead, fZ + fDirZ * fLead);
}

// The player left this scene: its puppet sails off on its course and keeps going.
void CoopNet_SeaPark(aref arP)
{
	CoopNet_Log("sea: player " + arP.id + " left this scene, its puppet sails away");
	int iChr = sti(arP.chr);
	float fAY = stf(arP.ay);
	DeleteAttribute(&Characters[iChr], "coopSpeed");
	Ship_SetTaskMove(PRIMARY_TASK, iChr, stf(arP.lx) + sin(fAY) * COOPNET_SEA_PARK, stf(arP.lz) + cos(fAY) * COOPNET_SEA_PARK);
	arP.parked = true;
}
// Logs this ship and the puppets every ~5 s (test runs).
void CoopNet_SeaLog()
{
	iCoopNetSeaLogTicks++;
	if (iCoopNetSeaLogTicks < 5) return;
	iCoopNetSeaLogTicks = 0;
	string sLine = "sea: me at " + pchar.Ship.Pos.x + " " + pchar.Ship.Pos.z + " speed " + pchar.Ship.Speed.z + " [" + pchar.location + "/" + worldMap.island + " zero " + worldMap.zeroX + " " + worldMap.zeroZ + "]";
	aref arList;
	makearef(arList, CoopNetSea);
	int iNum = GetAttributesNum(arList);
	for (int i = 0; i < iNum; i++)
	{
		aref arP = GetAttributeN(arList, i);
		if (!CoopNet_SeaLocalPos(arP) || !sti(arP.inSea)) continue;
		float fX = stf(arP.lx);
		float fZ = stf(arP.lz);
		ref rChr = &Characters[sti(arP.chr)];
		if (!CheckAttribute(rChr, "Ship.Pos.x")) continue;
		float fDX = fX - stf(rChr.Ship.Pos.x);
		float fDZ = fZ - stf(rChr.Ship.Pos.z);
		sLine = sLine + "; player " + arP.id + " sent " + arP.x + " " + arP.z + " at " + fX + " " + fZ + ", puppet " + sqrt(fDX * fDX + fDZ * fDZ) + " off";
	}
	CoopNet_Log(sLine);
}

