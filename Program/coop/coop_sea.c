// CoopNet sea: players at sea in the same place share their sea battles.
//
// Every player at sea sends all ships of its scene four times a second: its own fleet (flagship and
// companions, with their officers and skills counted on its machine) and the NPC ships of its scene.
// Each ship goes with its position, heading, speed, type, nation (flag) and hull state; the host
// passes the list on to everybody. Positions near an island are in that island's sea coordinates,
// which are the same for everybody there; in open sea each player's coordinates start where it left
// the world map, so they are sent as world map coordinates (sea / scale + the map point the scene
// was made around) and turned back into the receiver's own sea coordinates.
//
// Every ship is run by the machine it came from (its "owner"). Elsewhere it is a "puppet": an NPC
// with a ship of the same type, sailing (AI move task) to where the real ship will be a few seconds
// later, its speed matched to close the gap. Sides follow the flags: a puppet is friend or enemy by
// the relation between its nation and this player's flag (another player's fleet: nation against
// nation, so players under warring flags fight each other). A ship can't be moved or removed in a
// running sea scene, so a puppet whose ship leaves the scene sails away until it comes back.

#define COOPNET_MSG_SEA			130		// int id, string island, string mapIsland, float zeroX, zeroZ, int n, n x ship
										// ship: int idx, int baseType, int nation, string name, int flags, float x, z, ay, speed, hp
#define COOPNET_SEA_FLEET		1		// ship flags: a companion in the player's own fleet
#define COOPNET_SEA_FLAGSHIP	2		// the player's own ship (flags 0: an NPC ship)
#define COOPNET_SEA_TICKS		5		// report every 5 ticks = 0.25 s
#define COOPNET_SEA_STEER_TICKS	20		// new move task for puppets every second
#define COOPNET_SEA_RANGE		2500.0	// sea units: farther ships are not in this scene
#define COOPNET_SEA_LEAD		6.0		// seconds ahead a puppet sails to
#define COOPNET_SEA_CATCH_UP	0.15	// extra speed per sea unit a puppet is behind
#define COOPNET_SEA_STALE		3.0		// seconds without a report before a player has left the sea
#define COOPNET_SEA_PARK		10000.0	// how far a puppet sails off when its ship leaves

object CoopNetSea;	// p<id>: id, name, island, mapIsland, zeroX, zeroZ, at; flagship x, z, ay;
					// ships.s<idx>: idx, type, nation, name, flags, x, z, ay, speed, hp, seen; puppet: lx, lz, chr, inSea, parked, rel
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
	if (IsEntity(&worldMap))
	{
		iCoopNetSeaSteerTicks++;
		if (iCoopNetSeaSteerTicks < COOPNET_SEA_STEER_TICKS) return;
		iCoopNetSeaSteerTicks = 0;
		CoopNet_SeaOfferJoin();
		return;
	}

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

// ---- reports ---------------------------------------------------------------------------------

// The ships this machine runs: everything in the scene but puppets, forts and the dead.
bool CoopNet_SeaOwnShip(int iChr)
{
	if (iChr < 0 || iChr >= TOTAL_CHARACTERS) return false;
	ref rChr = &Characters[iChr];
	if (CheckAttribute(rChr, "coopPuppet")) return false;
	if (sti(rChr.Ship.Type) == SHIP_NOTUSED || sti(RealShips[sti(rChr.Ship.Type)].BaseType) == SHIP_FORT) return false;
	if (!CheckAttribute(rChr, "Ship.Pos.x") || LAi_IsDead(rChr)) return false;
	return true;
}

void CoopNet_SeaReport()
{
	string sIsland = pchar.location;
	float fScale = CoopNet_SeaScale();
	int i, iChr;
	int iNum = 0;
	for (i = 0; i < iNumShips; i++)
	{
		if (CoopNet_SeaOwnShip(Ships[i])) iNum++;
	}
	CoopNet_Begin(COOPNET_MSG_SEA);
	CoopNet_PutInt(CoopNet_MyId());
	CoopNet_PutString(sIsland);
	CoopNet_PutString(worldMap.island); // the scene's frame, for players joining this scene
	CoopNet_PutFloat(stf(worldMap.zeroX));
	CoopNet_PutFloat(stf(worldMap.zeroZ));
	CoopNet_PutInt(iNum);
	for (i = 0; i < iNumShips; i++)
	{
		iChr = Ships[i];
		if (!CoopNet_SeaOwnShip(iChr)) continue;
		ref rChr = &Characters[iChr];
		int iFlags = 0;
		if (iChr == nMainCharacterIndex) iFlags = COOPNET_SEA_FLAGSHIP;
		else
		{
			if (CheckAttribute(rChr, "SeaAI.Group.Name") && rChr.SeaAI.Group.Name == PLAYER_GROUP) iFlags = COOPNET_SEA_FLEET;
		}
		float fX = stf(rChr.Ship.Pos.x);
		float fZ = stf(rChr.Ship.Pos.z);
		if (sIsland == "")
		{
			fX = fX / fScale + stf(worldMap.zeroX);
			fZ = fZ / fScale + stf(worldMap.zeroZ);
		}
		float fSpeed = 0.0;
		if (CheckAttribute(rChr, "Ship.Speed.z")) fSpeed = stf(rChr.Ship.Speed.z);
		int iType = sti(rChr.Ship.Type);
		float fHP = stf(rChr.Ship.HP) / stf(RealShips[iType].HP);
		CoopNet_PutInt(iChr);
		CoopNet_PutInt(sti(RealShips[iType].BaseType));
		CoopNet_PutInt(sti(rChr.nation));
		CoopNet_PutString(rChr.Ship.Name);
		CoopNet_PutInt(iFlags);
		CoopNet_PutFloat(fX);
		CoopNet_PutFloat(fZ);
		CoopNet_PutFloat(stf(rChr.Ship.Ang.y));
		CoopNet_PutFloat(fSpeed);
		CoopNet_PutFloat(fHP);
	}
	if (CoopNet_State() == COOPNET_HOSTING) CoopNet_Send(COOPNET_ALL, COOPNET_UNRELIABLE);
	else CoopNet_Send(COOPNET_HOST_ID, COOPNET_UNRELIABLE);
}

void CoopNet_OnSea(int iFrom)
{
	int iId = CoopNet_GetInt();
	string sIsland = CoopNet_GetString();
	string sMapIsland = CoopNet_GetString();
	float fZeroX = CoopNet_GetFloat();
	float fZeroZ = CoopNet_GetFloat();
	int iNum = CoopNet_GetInt();
	bool bRelay = false;
	if (CoopNet_State() == COOPNET_HOSTING && iId == iFrom && iId != CoopNet_MyId())
	{
		bRelay = true;
		CoopNet_Begin(COOPNET_MSG_SEA);
		CoopNet_PutInt(iId);
		CoopNet_PutString(sIsland);
		CoopNet_PutString(sMapIsland);
		CoopNet_PutFloat(fZeroX);
		CoopNet_PutFloat(fZeroZ);
		CoopNet_PutInt(iNum);
	}
	bool bKeep = true;
	if (iId == CoopNet_MyId()) bKeep = false;
	if (CoopNet_State() == COOPNET_HOSTING && iId != iFrom) bKeep = false; // clients speak only for themselves
	string sKey = "p" + iId;
	aref arP, arShips;
	if (bKeep)
	{
		CoopNetSea.(sKey).id = iId;
		CoopNetSea.(sKey).name = CoopNet_PeerName(iId);
		CoopNetSea.(sKey).island = sIsland;
		CoopNetSea.(sKey).mapIsland = sMapIsland;
		CoopNetSea.(sKey).zeroX = fZeroX;
		CoopNetSea.(sKey).zeroZ = fZeroZ;
		CoopNetSea.(sKey).at = CoopNet_Seconds();
		CoopNetSea.(sKey).ships = "";
		makearef(arP, CoopNetSea.(sKey));
		makearef(arShips, arP.ships);
		int iOld = GetAttributesNum(arShips);
		for (int k = 0; k < iOld; k++)
		{
			aref arOld = GetAttributeN(arShips, k);
			arOld.seen = false;
		}
	}
	for (int i = 0; i < iNum; i++)
	{
		int iIdx = CoopNet_GetInt();
		int iType = CoopNet_GetInt();
		int iNation = CoopNet_GetInt();
		string sName = CoopNet_GetString();
		int iFlags = CoopNet_GetInt();
		float fX = CoopNet_GetFloat();
		float fZ = CoopNet_GetFloat();
		float fAY = CoopNet_GetFloat();
		float fSpeed = CoopNet_GetFloat();
		float fHP = CoopNet_GetFloat();
		if (bRelay)
		{
			CoopNet_PutInt(iIdx);
			CoopNet_PutInt(iType);
			CoopNet_PutInt(iNation);
			CoopNet_PutString(sName);
			CoopNet_PutInt(iFlags);
			CoopNet_PutFloat(fX);
			CoopNet_PutFloat(fZ);
			CoopNet_PutFloat(fAY);
			CoopNet_PutFloat(fSpeed);
			CoopNet_PutFloat(fHP);
		}
		if (!bKeep) continue;
		string sS = "s" + iIdx;
		arShips.(sS).idx = iIdx;
		arShips.(sS).type = iType;
		arShips.(sS).nation = iNation;
		arShips.(sS).name = sName;
		arShips.(sS).flags = iFlags;
		arShips.(sS).x = fX;
		arShips.(sS).z = fZ;
		arShips.(sS).ay = fAY;
		arShips.(sS).speed = fSpeed;
		arShips.(sS).hp = fHP;
		arShips.(sS).seen = true;
		if (!CheckAttribute(arShips, sS + ".inSea")) arShips.(sS).inSea = false;
		if (iFlags == COOPNET_SEA_FLAGSHIP)
		{
			arP.x = fX;
			arP.z = fZ;
			arP.ay = fAY;
		}
	}
	if (bRelay) CoopNet_Send(COOPNET_ALL, COOPNET_UNRELIABLE);
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
		if (!CheckAttribute(arP, "ships")) continue;
		aref arShips;
		makearef(arShips, arP.ships);
		int iShips = GetAttributesNum(arShips);
		for (int j = 0; j < iShips; j++)
		{
			aref arS = GetAttributeN(arShips, j);
			arS.inSea = false;
		}
	}
}

// ---- puppets ---------------------------------------------------------------------------------

// Where another player's ship is in this scene (arS.lx, arS.lz); false when it isn't in this scene.
// (A result through ref float parameters is unreliable in this script engine: the callee's own
// locals end up in them.)
bool CoopNet_SeaLocalPos(aref arP, aref arS)
{
	if (CoopNet_Seconds() - stf(arP.at) > COOPNET_SEA_STALE || !sti(arS.seen)) return false;
	string sMine = pchar.location;
	if (arP.island != sMine) return false;
	float fX = stf(arS.x);
	float fZ = stf(arS.z);
	if (sMine == "")
	{
		float fScale = CoopNet_SeaScale();
		fX = (fX - stf(worldMap.zeroX)) * fScale;
		fZ = (fZ - stf(worldMap.zeroZ)) * fScale;
	}
	arS.lx = fX;
	arS.lz = fZ;
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
		if (!CheckAttribute(arP, "ships")) continue;
		aref arShips;
		makearef(arShips, arP.ships);
		int iShips = GetAttributesNum(arShips);
		for (int j = 0; j < iShips; j++)
		{
			aref arS = GetAttributeN(arShips, j);
			if (!CoopNet_SeaLocalPos(arP, arS))
			{
				if (sti(arS.inSea) && !sti(arS.parked)) CoopNet_SeaPark(arP, arS);
				continue;
			}
			if (!sti(arS.inSea))
			{
				CoopNet_SeaCreatePuppet(arP, arS);
				continue;
			}
			CoopNet_SeaSides(arS);
			CoopNet_SeaSteer(arP, arS);
		}
	}
}

string CoopNet_SeaPuppetId(aref arP, aref arS)
{
	return "CoopPuppet_" + arP.id + "_" + arS.idx;
}

void CoopNet_SeaCreatePuppet(aref arP, aref arS)
{
	string sId = CoopNet_SeaPuppetId(arP, arS);
	int iChr = GetCharacterIndex(sId);
	if (iChr < 0) iChr = NPC_GenerateCharacter(sId, "officer_15", "man", "man", 10, sti(arS.nation), 0, false);
	if (iChr < 0) return;
	ref rChr = &Characters[iChr];
	int iType = sti(arS.type);
	if (!CheckAttribute(rChr, "coopShipBase") || sti(rChr.coopShipBase) != iType)
	{
		rChr.Ship.Type = GenerateShipExt(iType, true, rChr);
		rChr.coopShipBase = iType;
		SetBaseShipData(rChr);
	}
	rChr.coopPuppet = arP.id;
	rChr.Ship.Name = arS.name;
	rChr.nation = arS.nation;
	DeleteAttribute(rChr, "coopSpeed");
	DeleteAttribute(rChr, "AlwaysFriend");
	DeleteAttribute(rChr, "AlwaysEnemy");
	DeleteAttribute(rChr, "Ship.Sink");
	string sGroup = "CoopSea_" + arP.id + "_" + arS.idx;
	Group_FindOrCreateGroup(sGroup);
	Group_SetXZ_AY(sGroup, stf(arS.lx), stf(arS.lz), stf(arS.ay));
	Group_SetType(sGroup, "war");
	Group_AddCharacter(sGroup, sId);
	Group_SetGroupCommander(sGroup, sId);
	rChr.SeaAI.Group.Name = sGroup;
	Ship_Add2Sea(iChr, false, "");
	arS.chr = iChr;
	arS.inSea = true;
	arS.parked = false;
	arS.rel = -1;
	CoopNet_SeaSides(arS);
	CoopNet_Log("sea: ship " + arS.idx + " of player " + arP.id + " (" + arS.name + ", type " + iType + ", nation " + arS.nation + ") as " + sId + " at " + arS.lx + " " + arS.lz);
	CoopNet_SeaSteer(arP, arS);
}

// Friend or enemy by flags: another player's fleet against this player's flag, nation against
// nation; an NPC ship as the game would see it (its nation's relation to this player).
int CoopNet_SeaRelation(aref arS)
{
	int iNation = sti(arS.nation);
	if (sti(arS.flags) != 0) // a player's fleet
	{
		if (iNation == sti(pchar.nation)) return RELATION_FRIEND;
		return GetNationRelation(iNation, sti(pchar.nation));
	}
	return GetNationRelation2MainCharacter(iNation);
}

// Applies the puppet's sides to the ships this machine runs, when they changed.
void CoopNet_SeaSides(aref arS)
{
	int iRel = CoopNet_SeaRelation(arS);
	int iChr = sti(arS.chr);
	Characters[iChr].nation = arS.nation;
	if (iRel == sti(arS.rel)) return;
	arS.rel = iRel;
	int iNation = sti(arS.nation);
	for (int i = 0; i < iNumShips; i++)
	{
		int iOther = Ships[i];
		if (iOther < 0 || iOther == iChr || !CheckAttribute(&Characters[iOther], "Ship.Type")) continue;
		if (CheckAttribute(&Characters[iOther], "coopPuppet")) continue;
		int iOtherRel = iRel; // this player's fleet sides with its flag
		bool bMine = iOther == nMainCharacterIndex;
		if (CheckAttribute(&Characters[iOther], "SeaAI.Group.Name") && Characters[iOther].SeaAI.Group.Name == PLAYER_GROUP) bMine = true;
		if (!bMine)
		{
			iOtherRel = GetNationRelation(iNation, sti(Characters[iOther].nation));
			if (iNation == sti(Characters[iOther].nation)) iOtherRel = RELATION_FRIEND;
		}
		SetCharacterRelationBoth(iChr, iOther, iOtherRel);
	}
	UpdateRelations();
	CoopNet_Log("sea: " + Characters[iChr].id + " is now " + GetRelationName(iRel) + " to this player");
}

// The puppet sails for a point ahead of the real ship; its top speed is the ship's speed plus a
// share of the distance it is behind (Ship_UpdateParameters takes it from coopSpeed), so it catches
// up and doesn't overshoot. (Ships in a running sea scene can't be moved or removed: neither
// Group_SetXZ_AY nor writing Ship.Pos moves them.)
void CoopNet_SeaSteer(aref arP, aref arS)
{
	int iChr = sti(arS.chr);
	ref rChr = &Characters[iChr];
	if (!CheckAttribute(rChr, "Ship.Pos.x")) return;
	float fAY = stf(arS.ay);
	float fSpeed = stf(arS.speed);
	// the ship kept sailing since the report
	float fAge = CoopNet_Seconds() - stf(arP.at);
	float fDirX = sin(fAY);
	float fDirZ = cos(fAY);
	float fX = stf(arS.lx) + fDirX * fSpeed * fAge;
	float fZ = stf(arS.lz) + fDirZ * fSpeed * fAge;
	float fBehind = (fX - stf(rChr.Ship.Pos.x)) * fDirX + (fZ - stf(rChr.Ship.Pos.z)) * fDirZ;
	float fWanted = fSpeed + fBehind * COOPNET_SEA_CATCH_UP;
	float fMax = fSpeed * 2.5;
	if (fMax < 20.0) fMax = 20.0;
	if (fWanted > fMax) fWanted = fMax;
	if (fWanted < 0.0) fWanted = 0.0;
	rChr.coopSpeed = fWanted;
	rChr.Ship.MaxSpeedZ = fWanted;
	if (sti(arS.parked))
	{
		CoopNet_Log("sea: " + rChr.id + " is back in this scene");
		arS.parked = false;
	}
	float fLead = fSpeed * COOPNET_SEA_LEAD;
	if (fLead < 30.0) fLead = 30.0; // a goal right under the bow keeps the ship turning
	Ship_SetTaskMove(PRIMARY_TASK, iChr, fX + fDirX * fLead, fZ + fDirZ * fLead);
}

// The ship left this scene: its puppet sails off on its course and keeps going.
void CoopNet_SeaPark(aref arP, aref arS)
{
	int iChr = sti(arS.chr);
	CoopNet_Log("sea: " + Characters[iChr].id + " left this scene, it sails away");
	float fAY = stf(arS.ay);
	DeleteAttribute(&Characters[iChr], "coopSpeed");
	Ship_SetTaskMove(PRIMARY_TASK, iChr, stf(arS.lx) + sin(fAY) * COOPNET_SEA_PARK, stf(arS.lz) + cos(fAY) * COOPNET_SEA_PARK);
	arS.parked = true;
}

// Logs this ship and the puppets every ~5 s (test runs).
void CoopNet_SeaLog()
{
	iCoopNetSeaLogTicks++;
	if (iCoopNetSeaLogTicks < 5) return;
	iCoopNetSeaLogTicks = 0;
	string sLine = "sea: me at " + pchar.Ship.Pos.x + " " + pchar.Ship.Pos.z + " speed " + pchar.Ship.Speed.z;
	aref arList;
	makearef(arList, CoopNetSea);
	int iNum = GetAttributesNum(arList);
	for (int i = 0; i < iNum; i++)
	{
		aref arP = GetAttributeN(arList, i);
		if (!CheckAttribute(arP, "ships")) continue;
		aref arShips;
		makearef(arShips, arP.ships);
		int iShips = GetAttributesNum(arShips);
		for (int j = 0; j < iShips; j++)
		{
			aref arS = GetAttributeN(arShips, j);
			if (!sti(arS.inSea) || !CoopNet_SeaLocalPos(arP, arS)) continue;
			ref rChr = &Characters[sti(arS.chr)];
			if (!CheckAttribute(rChr, "Ship.Pos.x")) continue;
			float fDX = stf(arS.lx) - stf(rChr.Ship.Pos.x);
			float fDZ = stf(arS.lz) - stf(rChr.Ship.Pos.z);
			sLine = sLine + "; " + arP.id + "/" + arS.idx + " " + sqrt(fDX * fDX + fDZ * fDZ) + " off";
		}
	}
	CoopNet_Log(sLine);
}

// ---- joining another player's sea scene from the world map -----------------------------------
// On the world map, a player at sea within COOPNET_SEA_JOIN_RANGE map units is offered in the log;
// F9 (or sailing into its avatar) takes this player into that player's scene, next to its ship:
// wdmReloadToSea then asks CoopNet_SeaJoinLogin for the scene instead of the map's encounters.

#define COOPNET_SEA_JOIN_SIDE	200.0	// sea units to the side of the other player's ship
#define COOPNET_SEA_JOIN_RANGE	5.0		// world map units
#define COOPNET_SEA_JOIN_PAUSE	15.0	// seconds after the map opens before a join is offered
#define COOPNET_SEA_OFFER_EVERY	10.0	// seconds between repeats of the offer in the log

string sCoopNetSeaJoin = "";		// key in CoopNetSea of the player being joined
string sCoopNetSeaOffer = "";		// key of the player at sea that F9 would join now
float fCoopNetSeaOfferedAt = -100.0;

// Where a player at sea is on the world map: its position in open sea, else where it left the map.
bool CoopNet_SeaMapPos(aref arP)
{
	if (CoopNet_Seconds() - stf(arP.at) > COOPNET_SEA_STALE) return false;
	if (arP.island == "")
	{
		arP.mx = arP.x;
		arP.mz = arP.z;
		return true;
	}
	string sP = "p" + arP.id;
	if (!CheckAttribute(&CoopNetPlayers, sP + ".x") || !sti(CoopNetPlayers.(sP).hasMap)) return false;
	arP.mx = CoopNetPlayers.(sP).x;
	arP.mz = CoopNetPlayers.(sP).z;
	return true;
}

// On the world map: finds the nearest player at sea and offers to join it.
void CoopNet_SeaOfferJoin()
{
	string sBest = "";
	if (CoopNet_Seconds() - fCoopNetMapOpenedAt >= COOPNET_SEA_JOIN_PAUSE)
	{
		float fBest = COOPNET_SEA_JOIN_RANGE * COOPNET_SEA_JOIN_RANGE;
		float fMyX = stf(worldMap.playerShipX);
		float fMyZ = stf(worldMap.playerShipZ);
		aref arList;
		makearef(arList, CoopNetSea);
		int iNum = GetAttributesNum(arList);
		for (int i = 0; i < iNum; i++)
		{
			aref arP = GetAttributeN(arList, i);
			if (!CoopNet_SeaMapPos(arP)) continue;
			float fDX = stf(arP.mx) - fMyX;
			float fDZ = stf(arP.mz) - fMyZ;
			if (fDX * fDX + fDZ * fDZ > fBest) continue;
			fBest = fDX * fDX + fDZ * fDZ;
			sBest = GetAttributeName(arP);
		}
	}
	if (sBest == "")
	{
		sCoopNetSeaOffer = "";
		return;
	}
	if (sBest != sCoopNetSeaOffer || CoopNet_Seconds() - fCoopNetSeaOfferedAt > COOPNET_SEA_OFFER_EVERY)
	{
		Log_Info(CoopNetSea.(sBest).name + " is at sea nearby: F9 to join");
		fCoopNetSeaOfferedAt = CoopNet_Seconds();
	}
	sCoopNetSeaOffer = sBest;
	if (CoopNet_GetEnv("COOPNET_SEAJOIN") == "1") CoopNet_SeaJoinOffered(); // test instances
}

// F9 ("CoopJoinSea").
void CoopNet_SeaJoinOffered()
{
	if (!IsEntity(&worldMap) || sCoopNetSeaOffer == "" || wdmLockReload) return;
	CoopNet_SeaJoin(sCoopNetSeaOffer);
}

// From wdmEvent_ShipEncounter, when the ship ran into is a player's avatar.
void CoopNet_MeetAvatar(int iIndex)
{
	if (!wdmSetCurrentShipData(iIndex)) return;
	string sEnc = worldMap.encounter.id;
	if (!CheckAttribute(&worldMap, "encounters." + sEnc + ".coopAvatar")) return;
	if (CoopNet_Seconds() - fCoopNetMapOpenedAt < COOPNET_SEA_JOIN_PAUSE) return; // just came back from that scene
	string sKey = "p" + worldMap.encounters.(sEnc).coopAvatar;
	if (!CheckAttribute(&CoopNetSea, sKey + ".at")) return;
	if (CoopNet_Seconds() - stf(CoopNetSea.(sKey).at) > COOPNET_SEA_STALE) return; // not at sea
	if (wdmLockReload) return;
	CoopNet_SeaJoin(sKey);
}

void CoopNet_SeaJoin(string sKey)
{
	CoopNet_Log("sea: joining player " + CoopNetSea.(sKey).id + " at sea");
	Log_Info("Joining " + CoopNetSea.(sKey).name + " at sea");
	sCoopNetSeaJoin = sKey;
	sCoopNetSeaOffer = "";
	wdmReloadToSea();
}
// From wdmReloadToSea after WdmAddPlayerGroup: true when this reload joins another player's scene;
// then wdmLoginToSea and the map's frame (worldMap.island, zeroX/Z) are that scene's.
bool CoopNet_SeaJoinLogin()
{
	string sKey = sCoopNetSeaJoin;
	sCoopNetSeaJoin = "";
	if (sKey == "" || !CheckAttribute(&CoopNetSea, sKey + ".zeroX")) return false;
	aref arP;
	makearef(arP, CoopNetSea.(sKey));
	worldMap.island = arP.mapIsland;
	worldMap.zeroX = arP.zeroX;
	worldMap.zeroZ = arP.zeroZ;
	float fX = stf(arP.x);
	float fZ = stf(arP.z);
	if (arP.island == "")
	{
		float fScale = CoopNet_SeaScale();
		fX = (fX - stf(arP.zeroX)) * fScale;
		fZ = (fZ - stf(arP.zeroZ)) * fScale;
	}
	float fAY = stf(arP.ay);
	// abeam to port, same heading
	fX = fX - cos(fAY) * COOPNET_SEA_JOIN_SIDE;
	fZ = fZ + sin(fAY) * COOPNET_SEA_JOIN_SIDE;
	wdmLoginToSea.island = arP.island;
	wdmLoginToSea.playerGroup.x = fX;
	wdmLoginToSea.playerGroup.z = fZ;
	wdmLoginToSea.playerGroup.ay = fAY;
	CoopNet_Log("sea: logging in next to player " + arP.id + " at " + fX + " " + fZ + " (island '" + arP.island + "')");
	return true;
}