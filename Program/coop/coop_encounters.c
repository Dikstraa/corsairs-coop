// CoopNet encounters: the random ships of the world map (merchants, patrols, pirates, warring
// ships, floating barrels) are the same for everybody.
//
// The game creates them around the player's own ship, so every player still generates the ones
// around itself (the "owner"), but each one is registered with the host, which keeps the one list
// and passes every entry to everybody. On the world map every player shows the others' entries as
// "mirrors": encounters created at the owner's position, sailing to the same goal at the same speed
// (WorldMap.dll can't steer an existing encounter, so a mirror that drifts is re-created). The owner
// reports positions every 2 s while it is on the map; while it isn't, its ships stand still for it,
// and so do the mirrors. A mirror carries the owner's encounter data, so attacking it gives the same
// kind of squadron. An entry that disappears for anybody (attacked, arrived, expired) is deleted
// for everybody.
// Quest encounters, storms and the players' own avatars are not shared.

#define COOPNET_MSG_ENC_ADD			120	// string gid, int owner, string model, float x, z, gx, gz, k, speed, life, int hasGoal, int n, n x (string name, string value)
#define COOPNET_MSG_ENC_POS			121	// string gid, float x, z, gx, gz, k, speed, life, int hasGoal
#define COOPNET_MSG_ENC_DEL			122	// string gid
#define COOPNET_MSG_ENC_RESET		123	// host -> all: a new world, forget every entry
#define COOPNET_ENC_SCAN_TICKS		10		// every 10 ticks = 0.5 s
#define COOPNET_ENC_POS_PERIOD		2.0		// seconds between the owner's position reports
#define COOPNET_ENC_MAX_ERROR		30.0	// map units between a mirror and its ship before re-creation
#define COOPNET_ENC_LEAD			1.5		// seconds ahead a mirror starts, to make up for accelerating

object CoopNetEncs;	// g<gid>: gid, owner, model, x, z, gx, gz, k, speed, life, hasGoal, at, data; local: enc, seen, hadLocal, encParked, encGX, encGZ
int iCoopNetEncTicks = 0;
int iCoopNetEncSerial = 0;
int iCoopNetEncSalt = 0;
float fCoopNetEncLastPos = 0.0;
bool bCoopNetEncMapReady = false;	// mirrors from an earlier visit cleaned since the map opened
int iCoopNetEncLogTicks = 0;
int iCoopNetEncRecreated = 0;	// mirrors re-created since the last log line

void CoopNet_EncTick()
{
	int iState = CoopNet_State();
	if (!bCoopNetWorldInGame) return;
	if (iState != COOPNET_CONNECTED && iState != COOPNET_HOSTING)
	{
		// out of the session: the others' ships go, the own ones stay as plain encounters
		aref arList;
		makearef(arList, CoopNetEncs);
		if (GetAttributesNum(arList) > 0) CoopNet_EncReset();
		return;
	}
	if (iState == COOPNET_CONNECTED && !bCoopNetWorldSynced) return; // the host's list is on its way
	iCoopNetEncTicks++;
	if (iCoopNetEncTicks < COOPNET_ENC_SCAN_TICKS) return;
	iCoopNetEncTicks = 0;

	bool bMap = IsEntity(&worldMap);
	if (!bMap) bCoopNetEncMapReady = false;
	if (bMap && !bCoopNetEncMapReady)
	{
		// mirrors restored from the last visit are stale: they are made anew from the list
		CoopNet_EncPurgeMirrors();
		bCoopNetEncMapReady = true;
	}
	CoopNet_EncScan(bMap);
	if (!bMap) return;
	CoopNet_EncSyncMirrors();
	float fNow = CoopNet_Seconds();
	if (fNow - fCoopNetEncLastPos >= COOPNET_ENC_POS_PERIOD)
	{
		fCoopNetEncLastPos = fNow;
		CoopNet_EncReportOwn();
	}
	CoopNet_EncLogCounts();
}

// Logs the list every ~5 s on the map (test runs).
void CoopNet_EncLogCounts()
{
	iCoopNetEncLogTicks++;
	if (iCoopNetEncLogTicks < 10) return;
	iCoopNetEncLogTicks = 0;
	int iMe = CoopNet_MyId();
	int iOwn = 0;
	int iMirrors = 0;
	aref arEncs;
	makearef(arEncs, CoopNetEncs);
	int iNum = GetAttributesNum(arEncs);
	for (int i = 0; i < iNum; i++)
	{
		aref arE = GetAttributeN(arEncs, i);
		if (sti(arE.owner) == iMe) iOwn++;
		else
		{
			if (CheckAttribute(arE, "enc")) iMirrors++;
		}
	}
	CoopNet_Log("enc: " + iNum + " shared, " + iOwn + " own, " + iMirrors + " mirrors (" + iCoopNetEncRecreated + " re-created), " + wdmGetNumberShipEncounters() + " on the map");
	iCoopNetEncRecreated = 0;
}

// ---- the list ------------------------------------------------------------------------

string CoopNet_EncKey(string sGid)
{
	return "g" + sGid;
}

// Forgets every entry: deletes the mirrors and untags the own ships, which register anew.
void CoopNet_EncReset()
{
	aref arEncs;
	makearef(arEncs, worldMap.encounters);
	int iNum = GetAttributesNum(arEncs);
	for (int i = iNum - 1; i >= 0; i--)
	{
		aref arEnc = GetAttributeN(arEncs, i);
		if (CheckAttribute(arEnc, "coopMirror")) CoopNet_DeleteEncounter(GetAttributeName(arEnc));
		else DeleteAttribute(arEnc, "coopGid");
	}
	DeleteAttribute(&CoopNetEncs, "");
	iCoopNetEncSalt = rand(9999);
	bCoopNetEncMapReady = false;
}

void CoopNet_EncPurgeMirrors()
{
	aref arEncs;
	makearef(arEncs, worldMap.encounters);
	int iNum = GetAttributesNum(arEncs);
	for (int i = iNum - 1; i >= 0; i--)
	{
		aref arEnc = GetAttributeN(arEncs, i);
		if (CheckAttribute(arEnc, "coopMirror")) CoopNet_DeleteEncounter(GetAttributeName(arEnc));
	}
	makearef(arEncs, CoopNetEncs);
	iNum = GetAttributesNum(arEncs);
	for (i = 0; i < iNum; i++)
	{
		aref arE = GetAttributeN(arEncs, i);
		if (sti(arE.owner) == CoopNet_MyId()) continue;
		DeleteAttribute(arE, "enc");
		arE.hadLocal = false;
	}
}

// Matches the map's encounters with the list: registers new own ships, notices lost ones.
void CoopNet_EncScan(bool bMap)
{
	aref arList;
	makearef(arList, CoopNetEncs);
	int iNum = GetAttributesNum(arList);
	int i;
	for (i = 0; i < iNum; i++)
	{
		aref arE = GetAttributeN(arList, i);
		arE.seen = false;
	}

	aref arEncs;
	makearef(arEncs, worldMap.encounters);
	iNum = GetAttributesNum(arEncs);
	for (i = iNum - 1; i >= 0; i--)
	{
		aref arEnc = GetAttributeN(arEncs, i);
		string sEnc = GetAttributeName(arEnc);
		if (CheckAttribute(arEnc, "coopAvatar")) continue;
		if (CheckAttribute(arEnc, "coopGid"))
		{
			string sKey = CoopNet_EncKey(arEnc.coopGid);
			if (!CheckAttribute(&CoopNetEncs, sKey))
			{
				// deleted by somebody while this one stayed behind
				CoopNet_DeleteEncounter(sEnc);
				continue;
			}
			if (CheckAttribute(arEnc, "needDelete")) continue; // going away: lost below
			CoopNetEncs.(sKey).enc = sEnc;
			CoopNetEncs.(sKey).seen = true;
			CoopNetEncs.(sKey).hadLocal = true;
			continue;
		}
		if (bMap && CoopNet_EncShareable(arEnc)) CoopNet_EncRegister(arEnc, sEnc);
	}

	iNum = GetAttributesNum(arList);
	for (i = iNum - 1; i >= 0; i--)
	{
		arE = GetAttributeN(arList, i);
		if (sti(arE.seen) || !sti(arE.hadLocal)) continue;
		CoopNet_Log("enc: " + arE.gid + " gone here (owner " + arE.owner + ")");
		CoopNet_EncSendDel(arE.gid);
		DeleteAttribute(arList, GetAttributeName(arE));
	}
}

// A random ship the map made for this player.
bool CoopNet_EncShareable(aref arEnc)
{
	if (CheckAttribute(arEnc, "needDelete")) return false;
	if (CheckAttribute(arEnc, "quest")) return false;
	if (!CheckAttribute(arEnc, "encdata")) return false;
	if (!CheckAttribute(arEnc, "x") || !CheckAttribute(arEnc, "modelName")) return false; // not placed yet
	return true;
}

void CoopNet_EncRegister(aref arEnc, string sEnc)
{
	iCoopNetEncSerial++;
	string sGid = "" + CoopNet_MyId() + "_" + iCoopNetEncSalt + "_" + iCoopNetEncSerial;
	string sKey = CoopNet_EncKey(sGid);
	arEnc.coopGid = sGid;
	CoopNetEncs.(sKey).gid = sGid;
	CoopNetEncs.(sKey).owner = CoopNet_MyId();
	CoopNetEncs.(sKey).model = arEnc.modelName;
	CoopNetEncs.(sKey).enc = sEnc;
	CoopNetEncs.(sKey).seen = true;
	CoopNetEncs.(sKey).hadLocal = true;
	aref arE;
	makearef(arE, CoopNetEncs.(sKey));
	CoopNet_EncReadPosition(arE, arEnc);
	arE.data = "";
	aref arData, arSrc;
	makearef(arData, arE.data);
	makearef(arSrc, arEnc.encdata);
	CopyAttributes(arData, arSrc);
	CoopNet_Log("enc: registered " + sEnc + " as " + sGid + " (" + arE.model + ", " + arEnc.type + ")");
	CoopNet_EncSendAdd(arE, CoopNet_EncTarget());
}

void CoopNet_EncReadPosition(aref arE, aref arEnc)
{
	arE.x = arEnc.x;
	arE.z = arEnc.z;
	arE.hasGoal = CheckAttribute(arEnc, "gotox");
	if (sti(arE.hasGoal))
	{
		arE.gx = arEnc.gotox;
		arE.gz = arEnc.gotoz;
	}
	else
	{
		arE.gx = arEnc.x;
		arE.gz = arEnc.z;
	}
	arE.k = 1.0;
	if (CheckAttribute(arEnc, "kMaxSpeed")) arE.k = arEnc.kMaxSpeed;
	arE.speed = 0.0;
	if (CheckAttribute(arEnc, "speed")) arE.speed = arEnc.speed;
	arE.life = 1000.0;
	if (CheckAttribute(arEnc, "livetime")) arE.life = arEnc.livetime;
	arE.at = CoopNet_Seconds();
}

void CoopNet_EncReportOwn()
{
	int iMe = CoopNet_MyId();
	aref arList;
	makearef(arList, CoopNetEncs);
	int iNum = GetAttributesNum(arList);
	for (int i = 0; i < iNum; i++)
	{
		aref arE = GetAttributeN(arList, i);
		if (sti(arE.owner) != iMe || !CheckAttribute(arE, "enc")) continue;
		string sEnc = arE.enc;
		if (!CheckAttribute(&worldMap, "encounters." + sEnc + ".x")) continue;
		aref arEnc;
		makearef(arEnc, worldMap.encounters.(sEnc));
		CoopNet_EncReadPosition(arE, arEnc);
		CoopNet_EncSendPos(arE, CoopNet_EncTarget());
	}
}

// ---- mirrors ------------------------------------------------------------------------------

// The others' ships on this map: created when missing, re-created when they drift.
void CoopNet_EncSyncMirrors()
{
	int iMe = CoopNet_MyId();
	aref arList;
	makearef(arList, CoopNetEncs);
	int iNum = GetAttributesNum(arList);
	for (int i = 0; i < iNum; i++)
	{
		aref arE = GetAttributeN(arList, i);
		if (sti(arE.owner) == iMe) continue;
		bool bParked = CoopNet_EncParked(arE);
		float fX = stf(arE.x);
		float fZ = stf(arE.z);
		float fGX = stf(arE.gx);
		float fGZ = stf(arE.gz);
		float fDX = fGX - fX;
		float fDZ = fGZ - fZ;
		float fDist = sqrt(fDX * fDX + fDZ * fDZ);
		if (!bParked && fDist > 0.01)
		{
			// where the ship is now: it kept sailing since the report
			float fRun = stf(arE.speed) * (CoopNet_Seconds() - stf(arE.at));
			if (fRun > fDist) fRun = fDist;
			fX = fX + fDX / fDist * fRun;
			fZ = fZ + fDZ / fDist * fRun;
		}
		if (CheckAttribute(arE, "enc"))
		{
			string sEnc = arE.enc;
			if (CheckAttribute(arE, "encDriven") && sti(arE.encDriven))
			{
				// CoopNet.dll drives it: where the ship is now, and its course to its goal
				if (CoopNet_MapShipAlive(sEnc))
				{
					float fVX = 0.0;
					float fVZ = 0.0;
					float fAY = stf(arE.encAY);
					float fLeft = sqrt((fGX - fX) * (fGX - fX) + (fGZ - fZ) * (fGZ - fZ));
					if (!bParked && fLeft > 0.5)
					{
						fVX = (fGX - fX) / fLeft * stf(arE.speed);
						fVZ = (fGZ - fZ) / fLeft * stf(arE.speed);
						fAY = atan2(fGX - fX, fGZ - fZ);
						arE.encAY = fAY;
					}
					if (CoopNet_MapShipSet(sEnc, fX, fZ, fVX, fVZ, fAY) != 0) continue;
				}
				CoopNet_Log("enc: " + sEnc + " (" + arE.gid + ") no longer driven, made anew");
				arE.encDriven = false;
				DeleteAttribute(arE, "enc");
				CoopNet_DeleteEncounter(sEnc);
				iCoopNetEncRecreated++;
				CoopNet_EncCreateMirror(arE, fX, fZ, bParked);
				continue;
			}
			if (!CheckAttribute(&worldMap, "encounters." + sEnc + ".x")) continue; // placed next frame
			float fEX = fX - stf(worldMap.encounters.(sEnc).x);
			float fEZ = fZ - stf(worldMap.encounters.(sEnc).z);
			bool bRecreate = false;
			if (fEX * fEX + fEZ * fEZ > COOPNET_ENC_MAX_ERROR * COOPNET_ENC_MAX_ERROR) bRecreate = true;
			if (bParked != sti(arE.encParked)) bRecreate = true;
			fEX = fGX - stf(arE.encGX);
			fEZ = fGZ - stf(arE.encGZ);
			if (fEX * fEX + fEZ * fEZ > COOPNET_ENC_MAX_ERROR * COOPNET_ENC_MAX_ERROR) bRecreate = true;
			if (!bRecreate) continue;
			DeleteAttribute(arE, "enc");
			CoopNet_DeleteEncounter(sEnc);
			iCoopNetEncRecreated++;
		}
		CoopNet_EncCreateMirror(arE, fX, fZ, bParked);
	}
}

// A ship stands still while its owner is not on the map (its map isn't running), or when it has no goal.
bool CoopNet_EncParked(aref arE)
{
	if (!sti(arE.hasGoal)) return true;
	string sP = "p" + arE.owner;
	if (!CheckAttribute(&CoopNetPlayers, sP)) return false; // owner gone: the ship sails on by itself
	return sti(CoopNetPlayers.(sP).mode) != COOPNET_MODE_WORLDMAP;
}

void CoopNet_EncCreateMirror(aref arE, float fX, float fZ, bool bParked)
{
	float fGX = stf(arE.gx);
	float fGZ = stf(arE.gz);
	float fK = stf(arE.k);
	float fDX = fGX - fX;
	float fDZ = fGZ - fZ;
	float fDist = sqrt(fDX * fDX + fDZ * fDZ);
	float fStartX = fX;
	float fStartZ = fZ;
	if (bParked || fDist < 1.0)
	{
		// a goal far away at a crawl, so it never arrives (arrival deletes a merchant)
		fGX = fX + 1000.0;
		fGZ = fZ;
		fK = 0.0001;
	}
	else
	{
		float fLead = stf(arE.speed) * COOPNET_ENC_LEAD;
		if (fLead > fDist * 0.5) fLead = fDist * 0.5;
		fStartX = fX + fDX / fDist * fLead;
		fStartZ = fZ + fDZ / fDist * fLead;
	}
	string sModel = arE.model;
	// first as a ship CoopNet.dll drives: made where the ship is now, with a speed no other ship has
	// (by which the DLL finds it), a long life and a goal far off (the owner's word deletes it)
	arE.encDriven = false;
	iCoopNetMapShipKeys++;
	float fKey = 0.1 + 0.00013 * (1 + iCoopNetMapShipKeys - (iCoopNetMapShipKeys / 3000) * 3000);
	string sEnc = "";
	if (SendMessage(&worldMap, "lsffffff", MSG_WORLDMAP_CREATEENC_MER_XZ, sModel, fX, fZ, fX + 100000.0, fZ + 100000.0, fKey, 100000.0))
	{
		sEnc = worldMap.EncounterID1;
		if (sEnc != "")
		{
			if (CoopNet_MapShipBind(sEnc, fKey, fX, fZ) != 0)
			{
				arE.encDriven = true;
				arE.encAY = 0.0;
				if (fDist > 0.5) arE.encAY = atan2(fDX, fDZ);
				fStartX = fX;
				fStartZ = fZ;
			}
			else
			{
				CoopNet_DeleteEncounter(sEnc);
				sEnc = "";
			}
		}
	}
	if (sEnc == "")
	{
		// not found: an ordinary merchant on the ship's course, made anew when it drifts off
		if (!SendMessage(&worldMap, "lsffffff", MSG_WORLDMAP_CREATEENC_MER_XZ, sModel, fStartX, fStartZ, fGX, fGZ, fK, stf(arE.life))) return;
		sEnc = worldMap.EncounterID1;
		if (sEnc == "") return;
	}
	worldMap.encounters.(sEnc).coopGid = arE.gid;
	worldMap.encounters.(sEnc).coopMirror = true;
	worldMap.encounters.(sEnc).encdata = "";
	aref arData, arSrc;
	makearef(arData, worldMap.encounters.(sEnc).encdata);
	makearef(arSrc, arE.data);
	CopyAttributes(arData, arSrc);
	arE.enc = sEnc;
	arE.hadLocal = true;
	arE.seen = true;
	arE.encParked = bParked;
	arE.encGX = arE.gx;
	arE.encGZ = arE.gz;
}

// ---- traffic --------------------------------------------------------------------------------

int CoopNet_EncTarget()
{
	if (CoopNet_State() == COOPNET_HOSTING) return COOPNET_ALL;
	return COOPNET_HOST_ID;
}

void CoopNet_EncPutPosition(aref arE)
{
	CoopNet_PutFloat(stf(arE.x));
	CoopNet_PutFloat(stf(arE.z));
	CoopNet_PutFloat(stf(arE.gx));
	CoopNet_PutFloat(stf(arE.gz));
	CoopNet_PutFloat(stf(arE.k));
	CoopNet_PutFloat(stf(arE.speed));
	CoopNet_PutFloat(stf(arE.life));
	CoopNet_PutInt(sti(arE.hasGoal));
}

void CoopNet_EncGetPosition(aref arE)
{
	arE.x = CoopNet_GetFloat();
	arE.z = CoopNet_GetFloat();
	arE.gx = CoopNet_GetFloat();
	arE.gz = CoopNet_GetFloat();
	arE.k = CoopNet_GetFloat();
	arE.speed = CoopNet_GetFloat();
	arE.life = CoopNet_GetFloat();
	arE.hasGoal = CoopNet_GetInt();
	arE.at = CoopNet_Seconds();
}

void CoopNet_EncSendAdd(aref arE, int iTo)
{
	aref arData;
	makearef(arData, arE.data);
	int iNum = GetAttributesNum(arData);
	CoopNet_Begin(COOPNET_MSG_ENC_ADD);
	CoopNet_PutString(arE.gid);
	CoopNet_PutInt(sti(arE.owner));
	CoopNet_PutString(arE.model);
	CoopNet_EncPutPosition(arE);
	CoopNet_PutInt(iNum);
	for (int i = 0; i < iNum; i++)
	{
		aref arItem = GetAttributeN(arData, i);
		CoopNet_PutString(GetAttributeName(arItem));
		CoopNet_PutString(GetAttributeValue(arItem));
	}
	CoopNet_Send(iTo, COOPNET_RELIABLE);
}

void CoopNet_EncSendPos(aref arE, int iTo)
{
	CoopNet_Begin(COOPNET_MSG_ENC_POS);
	CoopNet_PutString(arE.gid);
	CoopNet_EncPutPosition(arE);
	CoopNet_Send(iTo, COOPNET_UNRELIABLE);
}

void CoopNet_EncSendDel(string sGid)
{
	CoopNet_Begin(COOPNET_MSG_ENC_DEL);
	CoopNet_PutString(sGid);
	CoopNet_Send(CoopNet_EncTarget(), COOPNET_RELIABLE);
}

// Host: the whole list to a client that just took the host's world.
void CoopNet_EncSendAll(int iTo)
{
	aref arList;
	makearef(arList, CoopNetEncs);
	int iNum = GetAttributesNum(arList);
	for (int i = 0; i < iNum; i++)
	{
		aref arE = GetAttributeN(arList, i);
		CoopNet_EncSendAdd(arE, iTo);
	}
}

// Host: a new world of its own (new game or load) - everybody forgets the old ships.
void CoopNet_EncWorldStarted()
{
	CoopNet_EncReset();
	if (CoopNet_State() != COOPNET_HOSTING) return;
	CoopNet_Begin(COOPNET_MSG_ENC_RESET);
	CoopNet_Send(COOPNET_ALL, COOPNET_RELIABLE);
}

void CoopNet_OnEncAdd(int iFrom)
{
	string sGid = CoopNet_GetString();
	int iOwner = CoopNet_GetInt();
	string sModel = CoopNet_GetString();
	string sKey = CoopNet_EncKey(sGid);
	bool bKnown = CheckAttribute(&CoopNetEncs, sKey);
	CoopNetEncs.(sKey).gid = sGid;
	CoopNetEncs.(sKey).owner = iOwner;
	CoopNetEncs.(sKey).model = sModel;
	aref arE;
	makearef(arE, CoopNetEncs.(sKey));
	CoopNet_EncGetPosition(arE);
	if (!bKnown)
	{
		arE.hadLocal = false;
		arE.seen = false;
	}
	DeleteAttribute(arE, "data");
	arE.data = "";
	int iNum = CoopNet_GetInt();
	for (int i = 0; i < iNum; i++)
	{
		string sName = CoopNet_GetString();
		string sValue = CoopNet_GetString();
		arE.data.(sName) = sValue;
	}
	if (CoopNet_State() == COOPNET_HOSTING && iFrom != COOPNET_HOST_ID) CoopNet_EncSendAdd(arE, COOPNET_ALL);
}

void CoopNet_OnEncPos(int iFrom)
{
	string sGid = CoopNet_GetString();
	string sKey = CoopNet_EncKey(sGid);
	if (!CheckAttribute(&CoopNetEncs, sKey)) return; // its ADD comes reliably, positions follow
	aref arE;
	makearef(arE, CoopNetEncs.(sKey));
	if (sti(arE.owner) == CoopNet_MyId()) return;
	CoopNet_EncGetPosition(arE);
	if (CoopNet_State() == COOPNET_HOSTING && iFrom != COOPNET_HOST_ID) CoopNet_EncSendPos(arE, COOPNET_ALL);
}

void CoopNet_OnEncDel(int iFrom)
{
	string sGid = CoopNet_GetString();
	string sKey = CoopNet_EncKey(sGid);
	if (!CheckAttribute(&CoopNetEncs, sKey)) return;
	if (CheckAttribute(&CoopNetEncs, sKey + ".enc"))
	{
		string sEnc = CoopNetEncs.(sKey).enc;
		if (CheckAttribute(&worldMap, "encounters." + sEnc + ".coopGid"))
		{
			if (worldMap.encounters.(sEnc).coopGid == sGid) CoopNet_DeleteEncounter(sEnc);
		}
	}
	DeleteAttribute(&CoopNetEncs, sKey);
	CoopNet_Log("enc: " + sGid + " gone (reported by " + iFrom + ")");
	if (CoopNet_State() == COOPNET_HOSTING && iFrom != COOPNET_HOST_ID)
	{
		CoopNet_Begin(COOPNET_MSG_ENC_DEL);
		CoopNet_PutString(sGid);
		CoopNet_Send(COOPNET_ALL, COOPNET_RELIABLE);
	}
}

// Returns true when the message was an encounter message.
bool CoopNet_EncDispatch(int iType, int iFrom)
{
	if (iType < COOPNET_MSG_ENC_ADD || iType > COOPNET_MSG_ENC_RESET) return false;
	if (!bCoopNetWorldInGame) return true;
	if (CoopNet_State() == COOPNET_CONNECTED && iFrom != COOPNET_HOST_ID) return true;
	if (iType == COOPNET_MSG_ENC_ADD) CoopNet_OnEncAdd(iFrom);
	if (iType == COOPNET_MSG_ENC_POS) CoopNet_OnEncPos(iFrom);
	if (iType == COOPNET_MSG_ENC_DEL) CoopNet_OnEncDel(iFrom);
	if (iType == COOPNET_MSG_ENC_RESET)
	{
		if (CoopNet_State() == COOPNET_CONNECTED) CoopNet_EncReset();
	}
	return true;
}
