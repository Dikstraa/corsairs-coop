// CoopNet presence: where every player is, and their ships on the world map.
//
// Every player sends its presence to the host twice a second; the host keeps the table and sends
// every entry (its own included) to everybody. While on the world map, every other player with a
// map position is shown as a ship ("avatar"): a world map encounter sailing where that player sails.
// WorldMap.dll ignores attribute writes to encounters, so an avatar can't be steered: it is created
// slightly ahead of the player (encounters start from a standstill) and re-created when it drifts
// too far from where the player is now (last report + its speed since) or the course changes.
// Every player measures its own map speed (the receiver can't: reports arrive unevenly).

#define COOPNET_MSG_PRESENCE		110		// int id, int mode, int hasMap, float x, float z, float ay, string place, string name, int busy (boarding), float vx, float vz
#define COOPNET_PRESENCE_TICKS		10		// send every 10 ticks = 0.5 s
#define COOPNET_PRESENCE_PERIOD		0.5
#define COOPNET_PRESENCE_STALE		20		// periods without news (10 s) before a player is dropped
#define COOPNET_AVATAR_MAX_ERROR	20.0	// map units between avatar and player before re-creation
#define COOPNET_AVATAR_MIN_COS		0.82	// course change (~35 deg) before re-creation
#define COOPNET_AVATAR_MOVING		0.5		// map units per second: below that the player is parked
#define COOPNET_AVATAR_SPEED_UNIT	14.8	// encounter speed at kSpeed 1 (measured)
#define COOPNET_AVATAR_LEAD			1.5		// seconds ahead an avatar starts, to make up for accelerating
#define COOPNET_AVATAR_MAX_GUESS	2.0		// seconds a report is carried forward at most

object CoopNetPlayers;	// p<id>: id, name, mode, hasMap, x, z, ay, vx, vz, at, place, busy, age; enc* while an avatar exists
int iCoopNetPresenceTicks = 0;
bool bCoopNetHasMapPos = false;
float fCoopNetMapX = 0.0;
float fCoopNetMapZ = 0.0;
float fCoopNetMapAY = 0.0;
float fCoopNetSpeedX = 0.0;		// our own map speed, measured between two presence sends
float fCoopNetSpeedZ = 0.0;
float fCoopNetSpeedFromX = 0.0;
float fCoopNetSpeedFromZ = 0.0;
float fCoopNetSpeedFromAt = -1.0;
bool bCoopNetAvatarsReady = false;	// stale avatars purged since the map was opened
float fCoopNetMapOpenedAt = 0.0;	// CoopNet_Seconds when the world map last opened

void CoopNet_PresenceTick()
{
	int iState = CoopNet_State();
	if (iState != COOPNET_CONNECTED && iState != COOPNET_HOSTING)
	{
		CoopNet_ForgetPlayers();
		return;
	}
	if (IsEntity(&worldMap))
	{
		fCoopNetMapX = stf(worldMap.playerShipX);
		fCoopNetMapZ = stf(worldMap.playerShipZ);
		fCoopNetMapAY = stf(worldMap.playerShipAY);
		bCoopNetHasMapPos = true;
	}
	iCoopNetPresenceTicks++;
	if (iCoopNetPresenceTicks >= COOPNET_PRESENCE_TICKS)
	{
		iCoopNetPresenceTicks = 0;
		int iMe = CoopNet_MyId();
		string sName = CoopNet_PeerName(iMe);
		string sPlace = CoopNet_PresencePlace();
		CoopNet_AgePresence();
		CoopNet_MeasureSpeed();
		CoopNet_StorePresence(iMe, iCoopNetMode, bCoopNetHasMapPos, fCoopNetMapX, fCoopNetMapZ, fCoopNetMapAY, sPlace, sName, fCoopNetSpeedX, fCoopNetSpeedZ);
		string sMe = "p" + iMe;
		// the cabin and the deck are boarding locations too, but nobody fights there
		CoopNetPlayers.(sMe).busy = bAbordageStarted && !bCabinStarted && !bDeckBoatStarted;
		if (iState == COOPNET_HOSTING) CoopNet_BroadcastPresence();
		else CoopNet_SendPresence(iMe, COOPNET_HOST_ID);
	}
	CoopNet_UpdateAvatars();
}

string CoopNet_PresencePlace()
{
	if (IsEntity(&worldMap)) return "worldmap";
	if (iCoopNetMode == COOPNET_MODE_MENU) return "";
	if (CheckAttribute(pchar, "location")) return pchar.location;
	return "";
}

// Our map speed over the real time since the last send; 0 off the map.
void CoopNet_MeasureSpeed()
{
	float fNow = CoopNet_Seconds();
	if (!IsEntity(&worldMap))
	{
		fCoopNetSpeedX = 0.0;
		fCoopNetSpeedZ = 0.0;
		fCoopNetSpeedFromAt = -1.0;
		return;
	}
	if (fCoopNetSpeedFromAt > 0.0 && fNow - fCoopNetSpeedFromAt > 0.1)
	{
		float fDt = fNow - fCoopNetSpeedFromAt;
		fCoopNetSpeedX = (fCoopNetMapX - fCoopNetSpeedFromX) / fDt;
		fCoopNetSpeedZ = (fCoopNetMapZ - fCoopNetSpeedFromZ) / fDt;
	}
	fCoopNetSpeedFromX = fCoopNetMapX;
	fCoopNetSpeedFromZ = fCoopNetMapZ;
	fCoopNetSpeedFromAt = fNow;
}

// ---- table -----------------------------------------------------------------------

void CoopNet_StorePresence(int iId, int iMode, bool bHasMap, float fX, float fZ, float fAY, string sPlace, string sName, float fVX, float fVZ)
{
	string sKey = "p" + iId;
	if (!CheckAttribute(&CoopNetPlayers, sKey + ".x"))
	{
		CoopNet_Log("presence: player " + iId + " '" + sName + "' seen");
	}
	CoopNetPlayers.(sKey).id = iId;
	CoopNetPlayers.(sKey).name = sName;
	CoopNetPlayers.(sKey).mode = iMode;
	CoopNetPlayers.(sKey).hasMap = bHasMap;
	CoopNetPlayers.(sKey).x = fX;
	CoopNetPlayers.(sKey).z = fZ;
	CoopNetPlayers.(sKey).ay = fAY;
	CoopNetPlayers.(sKey).vx = fVX;
	CoopNetPlayers.(sKey).vz = fVZ;
	CoopNetPlayers.(sKey).at = CoopNet_Seconds();
	CoopNetPlayers.(sKey).place = sPlace;
	CoopNetPlayers.(sKey).age = 0;
}

void CoopNet_AgePresence()
{
	aref arPlayers;
	makearef(arPlayers, CoopNetPlayers);
	int iNum = GetAttributesNum(arPlayers);
	for (int i = iNum - 1; i >= 0; i--)
	{
		aref arP = GetAttributeN(arPlayers, i);
		int iAge = sti(arP.age) + 1;
		arP.age = iAge;
		if (iAge > COOPNET_PRESENCE_STALE) CoopNet_DropPlayer(sti(arP.id));
	}
}

void CoopNet_DropPlayer(int iId)
{
	string sKey = "p" + iId;
	if (!CheckAttribute(&CoopNetPlayers, sKey)) return;
	aref arP;
	makearef(arP, CoopNetPlayers.(sKey));
	CoopNet_AvatarRemove(arP);
	CoopNet_Log("presence: player " + iId + " gone");
	DeleteAttribute(&CoopNetPlayers, sKey);
}

void CoopNet_ForgetPlayers()
{
	aref arPlayers;
	makearef(arPlayers, CoopNetPlayers);
	int iNum = GetAttributesNum(arPlayers);
	for (int i = iNum - 1; i >= 0; i--)
	{
		aref arP = GetAttributeN(arPlayers, i);
		CoopNet_AvatarRemove(arP);
	}
	DeleteAttribute(&CoopNetPlayers, "");
	bCoopNetHasMapPos = false;
}

// ---- traffic ---------------------------------------------------------------------

void CoopNet_SendPresence(int iId, int iTo)
{
	string sKey = "p" + iId;
	if (!CheckAttribute(&CoopNetPlayers, sKey + ".x")) return;
	aref arP;
	makearef(arP, CoopNetPlayers.(sKey));
	CoopNet_Begin(COOPNET_MSG_PRESENCE);
	CoopNet_PutInt(iId);
	CoopNet_PutInt(sti(arP.mode));
	CoopNet_PutInt(sti(arP.hasMap));
	CoopNet_PutFloat(stf(arP.x));
	CoopNet_PutFloat(stf(arP.z));
	CoopNet_PutFloat(stf(arP.ay));
	CoopNet_PutString(arP.place);
	CoopNet_PutString(arP.name);
	int iBusy = 0;
	if (CheckAttribute(arP, "busy")) iBusy = sti(arP.busy);
	CoopNet_PutInt(iBusy);
	CoopNet_PutFloat(stf(arP.vx));
	CoopNet_PutFloat(stf(arP.vz));
	CoopNet_Send(iTo, COOPNET_UNRELIABLE);
}

// Host: everybody gets every entry, the host's own included.
void CoopNet_BroadcastPresence()
{
	aref arPlayers;
	makearef(arPlayers, CoopNetPlayers);
	int iNum = GetAttributesNum(arPlayers);
	for (int i = 0; i < iNum; i++)
	{
		aref arP = GetAttributeN(arPlayers, i);
		CoopNet_SendPresence(sti(arP.id), COOPNET_ALL);
	}
}

void CoopNet_OnPresence(int iFrom)
{
	int iId = CoopNet_GetInt();
	int iMode = CoopNet_GetInt();
	int iHasMap = CoopNet_GetInt();
	float fX = CoopNet_GetFloat();
	float fZ = CoopNet_GetFloat();
	float fAY = CoopNet_GetFloat();
	string sPlace = CoopNet_GetString();
	string sName = CoopNet_GetString();
	int iBusy = CoopNet_GetInt();
	float fVX = CoopNet_GetFloat();
	float fVZ = CoopNet_GetFloat();
	if (iId == CoopNet_MyId()) return;
	if (CoopNet_State() == COOPNET_HOSTING && iId != iFrom) return; // clients speak only for themselves
	CoopNet_StorePresence(iId, iMode, iHasMap, fX, fZ, fAY, sPlace, sName, fVX, fVZ);
	string sKey = "p" + iId;
	CoopNetPlayers.(sKey).busy = iBusy;
}

// ---- avatars on the world map ----------------------------------------------------

void CoopNet_UpdateAvatars()
{
	if (!IsEntity(&worldMap))
	{
		// the map released its encounters; their attributes are purged when it opens again
		bCoopNetAvatarsReady = false;
		CoopNet_ForgetAvatarIds();
		return;
	}
	if (!bCoopNetAvatarsReady)
	{
		CoopNet_PurgeAvatarEncounters();
		bCoopNetAvatarsReady = true;
		fCoopNetMapOpenedAt = CoopNet_Seconds();
	}
	int iMe = CoopNet_MyId();
	aref arPlayers;
	makearef(arPlayers, CoopNetPlayers);
	int iNum = GetAttributesNum(arPlayers);
	for (int i = 0; i < iNum; i++)
	{
		aref arP = GetAttributeN(arPlayers, i);
		if (sti(arP.id) == iMe) continue;
		if (sti(arP.mode) == COOPNET_MODE_MENU || sti(arP.hasMap) == 0)
		{
			CoopNet_AvatarRemove(arP);
			continue;
		}
		CoopNet_AvatarSync(arP);
	}
}

void CoopNet_AvatarSync(aref arP)
{
	float fX = stf(arP.x);
	float fZ = stf(arP.z);
	float fVX = stf(arP.vx);
	float fVZ = stf(arP.vz);
	float fSpeed = sqrt(fVX * fVX + fVZ * fVZ);
	bool bMoving = fSpeed >= COOPNET_AVATAR_MOVING;
	if (bMoving)
	{
		// where the player is now: it kept sailing since the report
		float fAge = CoopNet_Seconds() - stf(arP.at);
		if (fAge > COOPNET_AVATAR_MAX_GUESS) fAge = COOPNET_AVATAR_MAX_GUESS;
		fX = fX + fVX * fAge;
		fZ = fZ + fVZ * fAge;
	}

	if (CheckAttribute(arP, "enc"))
	{
		string sEnc = arP.enc;
		bool bRecreate = true;
		// a new encounter gets its position when the map runs next (not while a dialog holds it)
		if (CheckAttribute(&worldMap, "encounters." + sEnc) && !CheckAttribute(&worldMap, "encounters." + sEnc + ".x")) return;
		if (CheckAttribute(&worldMap, "encounters." + sEnc + ".x"))
		{
			float fDX = fX - stf(worldMap.encounters.(sEnc).x);
			float fDZ = fZ - stf(worldMap.encounters.(sEnc).z);
			bRecreate = false;
			if (fDX * fDX + fDZ * fDZ > COOPNET_AVATAR_MAX_ERROR * COOPNET_AVATAR_MAX_ERROR) bRecreate = true;
			if (bMoving != sti(arP.encMoving)) bRecreate = true;
			if (bMoving && sti(arP.encMoving))
			{
				float fOldVX = stf(arP.encVX);
				float fOldVZ = stf(arP.encVZ);
				float fCos = (fVX * fOldVX + fVZ * fOldVZ) / (fSpeed * sqrt(fOldVX * fOldVX + fOldVZ * fOldVZ));
				if (fCos < COOPNET_AVATAR_MIN_COS) bRecreate = true;
			}
		}
		if (!bRecreate) return;
		CoopNet_AvatarRemove(arP);
	}
	CoopNet_AvatarCreate(arP, fX, fZ, fVX, fVZ, fSpeed, bMoving);
}

void CoopNet_AvatarCreate(aref arP, float fX, float fZ, float fVX, float fVZ, float fSpeed, bool bMoving)
{
	float fStartX = fX;
	float fStartZ = fZ;
	float fGoX, fGoZ, fK;
	if (bMoving)
	{
		fStartX = fX + fVX * COOPNET_AVATAR_LEAD;
		fStartZ = fZ + fVZ * COOPNET_AVATAR_LEAD;
		fGoX = fX + fVX * 120.0;
		fGoZ = fZ + fVZ * 120.0;
		fK = fSpeed / COOPNET_AVATAR_SPEED_UNIT;
		if (fK > 2.0) fK = 2.0;
	}
	else
	{
		// parked: a goal far ahead at a crawl, so it never arrives (arrival deletes a merchant)
		float fAY = stf(arP.ay);
		fGoX = fX + sin(fAY) * 1000.0;
		fGoZ = fZ + cos(fAY) * 1000.0;
		fK = 0.0001;
	}
	SendMessage(&worldMap, "lsffffff", MSG_WORLDMAP_CREATEENC_MER_XZ, "ship", fStartX, fStartZ, fGoX, fGoZ, fK, 100000.0);
	string sEnc = worldMap.EncounterID1;
	worldMap.encounters.(sEnc).coopAvatar = arP.id;
	arP.enc = sEnc;
	arP.encMoving = bMoving;
	arP.encVX = fVX;
	arP.encVZ = fVZ;
	CoopNet_Log("avatar: player " + arP.id + " as " + sEnc + " at " + fStartX + " " + fStartZ + ", speed " + fSpeed);
}

void CoopNet_AvatarRemove(aref arP)
{
	if (!CheckAttribute(arP, "enc")) return;
	string sEnc = arP.enc;
	DeleteAttribute(arP, "enc");
	if (IsEntity(&worldMap)) CoopNet_DeleteEncounter(sEnc);
}

// needDelete + deleteUpdate makes WorldMap.dll drop the encounter when the map runs next; its
// attributes must stay till then (the map drops them itself when it opens next, as the game does).
void CoopNet_DeleteEncounter(string sEnc)
{
	if (!CheckAttribute(&worldMap, "encounters." + sEnc)) return;
	if (!IsEntity(&worldMap))
	{
		DeleteAttribute(&worldMap, "encounters." + sEnc);
		return;
	}
	worldMap.encounters.(sEnc).needDelete = "coop";
	worldMap.deleteUpdate = "";
}

void CoopNet_ForgetAvatarIds()
{
	aref arPlayers;
	makearef(arPlayers, CoopNetPlayers);
	int iNum = GetAttributesNum(arPlayers);
	for (int i = 0; i < iNum; i++)
	{
		aref arP = GetAttributeN(arPlayers, i);
		DeleteAttribute(arP, "enc");
	}
}

// Avatars left in worldMap.encounters from an earlier visit (or a save) would be restored by the
// map as plain merchants.
void CoopNet_PurgeAvatarEncounters()
{
	aref arEncs;
	makearef(arEncs, worldMap.encounters);
	int iNum = GetAttributesNum(arEncs);
	for (int i = iNum - 1; i >= 0; i--)
	{
		aref arEnc = GetAttributeN(arEncs, i);
		if (CheckAttribute(arEnc, "coopAvatar")) CoopNet_DeleteEncounter(GetAttributeName(arEnc));
	}
}

// For wdmEvent_ShipEncounter: true when the ship we ran into is another player's avatar.
bool CoopNet_IsAvatarEncounter(int iIndex)
{
	if (!wdmSetCurrentShipData(iIndex)) return false;
	string sEnc = worldMap.encounter.id;
	return CheckAttribute(&worldMap, "encounters." + sEnc + ".coopAvatar");
}
