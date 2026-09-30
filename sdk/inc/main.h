// Script Hook V exports. Declarations must match the mangled names exported by
// ScriptHookV.dll (see sdk/lib/ScriptHookV.def).
#pragma once

#include <windows.h>

#define IMPORT __declspec(dllimport)

// Textures
IMPORT int createTexture(const char* texFileName);
IMPORT void drawTexture(int id, int index, int level, int time,
	float sizeX, float sizeY, float centerX, float centerY,
	float posX, float posY, float rotation, float screenHeightScaleFactor,
	float r, float g, float b, float a);

// D3D present callback, called from the render thread.
typedef void (*PresentCallback)(void*);
IMPORT void presentCallbackRegister(PresentCallback cb);
IMPORT void presentCallbackUnregister(PresentCallback cb);

// Keyboard
typedef void (*KeyboardHandler)(DWORD key, WORD repeats, BYTE scanCode,
	BOOL isExtended, BOOL isWithAlt, BOOL wasDownBefore, BOOL isUpNow);
IMPORT void keyboardHandlerRegister(KeyboardHandler handler);
IMPORT void keyboardHandlerUnregister(KeyboardHandler handler);

// Scripts
IMPORT void scriptWait(DWORD time);
IMPORT void scriptRegister(HMODULE module, void (*LP_SCRIPT_MAIN)());
IMPORT void scriptRegisterAdditionalThread(HMODULE module, void (*LP_SCRIPT_MAIN)());
IMPORT void scriptUnregister(HMODULE module);
IMPORT void scriptUnregister(void (*LP_SCRIPT_MAIN)()); // deprecated
IMPORT bool scriptsAreLaunchedUsingReloading();

// Natives
IMPORT void nativeInit(UINT64 hash);
IMPORT void nativePush64(UINT64 val);
IMPORT PUINT64 nativeCall();
IMPORT bool nativeCanExecuteInThisContext();

static void WAIT(DWORD time) { scriptWait(time); }
static void TERMINATE() { WAIT(MAXDWORD); }

// Globals / world
IMPORT UINT64* getGlobalPtr(int globalId);
IMPORT int worldGetAllVehicles(int* arr, int arrSize);
IMPORT int worldGetAllPeds(int* arr, int arrSize);
IMPORT int worldGetAllObjects(int* arr, int arrSize);
IMPORT int worldGetAllPickups(int* arr, int arrSize);
IMPORT BYTE* getScriptHandleBaseAddress(int handle);

enum eGameVersion : int;
IMPORT eGameVersion getGameVersion();
