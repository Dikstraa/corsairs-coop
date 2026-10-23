// CoopNet native library (modules/CoopNet.dll), sources in CoopMod/src
native int CoopNet_Version();
native float CoopNet_AddF(float a, float b);
native string CoopNet_Echo(string s);
native int CoopNet_Log(string s);

#library "COOPNET"

void CoopNet_SelfTest()
{
	int iVer = CoopNet_Version();
	float fSum = CoopNet_AddF(1.5, 2.25);
	string sEcho = CoopNet_Echo("ahoy");
	string sRes = "CoopNet selftest: version=" + iVer + " add=" + fSum + " echo=" + sEcho;
	Trace(sRes);
	CoopNet_Log(sRes);
}
