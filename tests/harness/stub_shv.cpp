// Fake ScriptHookV.dll for running PhoneLink.asi outside the game.
// Natives return 0 except IS_PLAYER_PLAYING (1); interesting calls are printed.
#include <windows.h>

#include <cstdio>
#include <map>
#include <mutex>

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
EXPORT PUINT64 nativeCall()
{
	g_result[0] = (g_hash == 0x5E9564D8246B909A) ? 1 : 0;
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
EXPORT void harnessRunScript() { if (g_scriptMain) g_scriptMain(); }
EXPORT void harnessKey(DWORD vk)
{
	if (g_keyboard)
		g_keyboard(vk, 1, 0, FALSE, FALSE, FALSE, FALSE);
}
