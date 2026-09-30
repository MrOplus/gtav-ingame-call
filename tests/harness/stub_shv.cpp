// Fake ScriptHookV.dll for running PhoneLink.asi outside the game.
// Natives return 0 except IS_PLAYER_PLAYING (1); interesting calls are printed.
#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#define EXPORT __declspec(dllexport)

static void (*g_scriptMain)() = nullptr;
static void (*g_keyboard)(DWORD, WORD, BYTE, BOOL, BOOL, BOOL, BOOL) = nullptr;
static UINT64 g_hash = 0;
static UINT64 g_args[32];
static int g_argc = 0;
static UINT64 g_result[4];

static const std::map<UINT64, const char*> g_names = {
	{0xF9E56683CA8E11A5, "PLAY_PED_RINGTONE"},
	{0x6C5AE23EFA885092, "STOP_PED_RINGTONE"},
	{0x67C540AA08E4A6F5, "PLAY_SOUND_FRONTEND"},
	{0x1CCD9A37359072CF, "END_TEXT_COMMAND_THEFEED_POST_MESSAGETEXT"},
	{0xBD2A8EC3AF4DE7DB, "TASK_USE_MOBILE_PHONE"},
	{0x6C188BE134E074AA, "ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME"},
	{0x5E9564D8246B909A, "IS_PLAYER_PLAYING"},
};

static bool g_inPrint = false; // only echo text components that belong to feed/subtitle posts

// ---- simulated game phone (driven by harness commands) ------------------------------
static bool g_phoneUp = false;       // cellphone_flashhand running
static bool g_contactsApp = false;   // appcontacts running
static int g_selectSlot = -1;        // pending "select" press -> GET_CURRENT_SELECTION result
static bool g_selectFire = false;
static bool g_cancelFire = false;
static bool g_inScaleform = false;
static std::string g_sfLine, g_lastSfLine;

static uint32_t joaat(const char* s)
{
	uint32_t h = 0;
	for (; *s; ++s)
	{
		h += static_cast<uint8_t>(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s);
		h += h << 10;
		h ^= h >> 6;
	}
	h += h << 3;
	h ^= h >> 11;
	h += h << 15;
	return h;
}

static void sfPrint(const std::string& line)
{
	if (line == g_lastSfLine)
		return; // the contacts list is re-sent every frame
	g_lastSfLine = line;
	printf("[scaleform] %s\n", line.c_str());
	fflush(stdout);
}

EXPORT int createTexture(const char*) { return 0; }
EXPORT void drawTexture(int, int, int, int, float, float, float, float, float, float, float, float, float, float, float, float) {}
EXPORT void presentCallbackRegister(void (*)(void*)) {}
EXPORT void presentCallbackUnregister(void (*)(void*)) {}
EXPORT void keyboardHandlerRegister(void (*h)(DWORD, WORD, BYTE, BOOL, BOOL, BOOL, BOOL)) { g_keyboard = h; }
EXPORT void keyboardHandlerUnregister(void (*)(DWORD, WORD, BYTE, BOOL, BOOL, BOOL, BOOL)) { g_keyboard = nullptr; }
EXPORT void scriptWait(DWORD ms) { Sleep(ms ? ms : 16); }
EXPORT void scriptRegister(HMODULE, void (*fn)()) { g_scriptMain = fn; }
EXPORT void scriptRegisterAdditionalThread(HMODULE, void (*)()) {}
EXPORT void scriptUnregister(HMODULE) {}
EXPORT void scriptUnregister(void (*)()) {}
EXPORT bool scriptsAreLaunchedUsingReloading() { return false; }
EXPORT bool nativeCanExecuteInThisContext() { return true; }
EXPORT UINT64* getGlobalPtr(int) { static UINT64 g; return &g; }
EXPORT int worldGetAllVehicles(int*, int) { return 0; }
EXPORT int worldGetAllPeds(int*, int) { return 0; }
EXPORT int worldGetAllObjects(int*, int) { return 0; }
EXPORT int worldGetAllPickups(int*, int) { return 0; }
EXPORT BYTE* getScriptHandleBaseAddress(int) { return nullptr; }
enum eGameVersion : int { VER_UNK = -1 };
EXPORT eGameVersion getGameVersion() { return VER_UNK; }

EXPORT void nativeInit(UINT64 hash) { g_hash = hash; g_argc = 0; }
EXPORT void nativePush64(UINT64 v) { if (g_argc < 32) g_args[g_argc++] = v; }
static bool phoneNative()
{
	auto str = [](int i) { return reinterpret_cast<const char*>(g_args[i]); };
	switch (g_hash)
	{
	case 0x2C83A9DA6BFFC4F9: // GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH
	{
		uint32_t h = static_cast<uint32_t>(g_args[0]);
		g_result[0] = (h == joaat("cellphone_flashhand") && g_phoneUp) || (h == joaat("appcontacts") && g_contactsApp);
		return true;
	}
	case 0x11FE353CF9733E6F: // REQUEST_SCALEFORM_MOVIE
		sfPrint(std::string("REQUEST ") + str(0));
		g_result[0] = 7;
		return true;
	case 0x85F01B8D5B90570E: // HAS_SCALEFORM_MOVIE_LOADED
	case 0xE6CC9F3BA0FB9EF1: // HAS_SCRIPT_LOADED
	case 0x768FF8961BA904D6: // IS_SCALEFORM_MOVIE_METHOD_RETURN_VALUE_READY
		g_result[0] = 1;
		return true;
	case 0xC50AA39A577AF886: // END_SCALEFORM_MOVIE_METHOD_RETURN_VALUE
		g_inScaleform = false;
		sfPrint(g_sfLine);
		g_result[0] = 99;
		return true;
	case 0x2DE7EFA66B906036: // GET_SCALEFORM_MOVIE_METHOD_RETURN_VALUE_INT
		g_result[0] = static_cast<UINT64>(g_selectSlot);
		return true;
	case 0x580417101DDB492F: // IS_CONTROL_JUST_PRESSED
	case 0x91AEF906BCA88877: // IS_DISABLED_CONTROL_JUST_PRESSED
		if (g_args[1] == 176 && g_selectFire) { g_selectFire = false; g_result[0] = 1; return true; }
		if (g_args[1] == 177 && g_cancelFire) { g_cancelFire = false; g_result[0] = 1; return true; }
		g_result[0] = 0;
		return true;
	case 0xF6E48914C7A8694E: // BEGIN_SCALEFORM_MOVIE_METHOD
		g_inScaleform = true;
		g_sfLine = str(1);
		return true;
	case 0xC3D0841A0CC546A6: // SCALEFORM_MOVIE_METHOD_ADD_PARAM_INT
		g_sfLine += " " + std::to_string(static_cast<int>(g_args[0]));
		return true;
	case 0x80338406F3475E55: // BEGIN_TEXT_COMMAND_SCALEFORM_STRING
		if (strcmp(str(0), "STRING") != 0 && strcmp(str(0), "CELL_2000") != 0)
			g_sfLine += std::string(" ") + str(0);
		return true;
	case 0x6C188BE134E074AA: // ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME
	case 0x761B77454205A61D: // ADD_TEXT_COMPONENT_SUBSTRING_PHONE_NUMBER
		if (!g_inScaleform)
			return false; // feed/subtitle text: handled below
		g_sfLine += std::string(" '") + str(0) + "'";
		return true;
	case 0xC6796A8FFA375E53: // END_SCALEFORM_MOVIE_METHOD
		g_inScaleform = false;
		sfPrint(g_sfLine);
		return true;
	case 0x9DC711BC69C548DF: // TERMINATE_ALL_SCRIPTS_WITH_THIS_NAME
		printf("[native] TERMINATE %s\n", str(0));
		fflush(stdout);
		if (!strcmp(str(0), "appcontacts")) g_contactsApp = false;
		if (!strcmp(str(0), "cellphone_flashhand")) g_phoneUp = false;
		return true;
	case 0xE81651AD79516E48: // START_NEW_SCRIPT
		printf("[native] START_NEW_SCRIPT %s\n", str(0));
		fflush(stdout);
		return true;
	case 0x430386FE9BF80B45: // GET_SOUND_ID
		g_result[0] = 5;
		return true;
	default:
		return false;
	}
}

EXPORT PUINT64 nativeCall()
{
	g_result[0] = (g_hash == 0x5E9564D8246B909A) ? 1 : 0;
	if (phoneNative())
		return g_result;
	auto it = g_names.find(g_hash);
	if (it != g_names.end() && g_hash != 0x5E9564D8246B909A)
	{
		if (g_hash == 0x6C188BE134E074AA)
		{
			if (g_inPrint)
				printf("    text: %s\n", reinterpret_cast<const char*>(g_args[0]));
		}
		else if (g_hash == 0x1CCD9A37359072CF)
			printf("[native] FEED sender='%s' subject='%s'\n", (const char*)g_args[4], (const char*)g_args[5]);
		else if (g_hash == 0xF9E56683CA8E11A5 || g_hash == 0x67C540AA08E4A6F5)
			printf("[native] %s '%s'\n", it->second, (const char*)g_args[g_hash == 0xF9E56683CA8E11A5 ? 0 : 1]);
		else
			printf("[native] %s\n", it->second);
		fflush(stdout);
	}
	// BEGIN_TEXT_COMMAND_THEFEED_POST / BEGIN_TEXT_COMMAND_PRINT start an echoed text block
	if (g_hash == 0x202709F4C58A0424 || g_hash == 0xB87A37EEB7FAA67D)
		g_inPrint = true;
	if (g_hash == 0x2ED7843F8F801023 || g_hash == 0x9D77056A530643F6 || g_hash == 0x25FBB336DF1804CB)
		g_inPrint = false;
	if (g_hash == 0x9D77056A530643F6)
		printf("[native] SUBTITLE shown\n");
	return g_result;
}

// Harness helpers
// op: 0 phone down, 1 phone up (home screen), 2 contacts app open, 3 select slot <arg>, 4 cancel
EXPORT void harnessPhone(int op, int arg)
{
	switch (op)
	{
	case 0: g_phoneUp = g_contactsApp = false; break;
	case 1: g_phoneUp = true; g_contactsApp = false; break;
	case 2: g_phoneUp = g_contactsApp = true; break;
	case 3: g_selectSlot = arg; g_selectFire = true; break;
	case 4: g_cancelFire = true; break;
	}
}

EXPORT void harnessRunScript() { if (g_scriptMain) g_scriptMain(); }
EXPORT void harnessKey(DWORD vk)
{
	if (g_keyboard)
		g_keyboard(vk, 1, 0, FALSE, FALSE, FALSE, FALSE);
}
