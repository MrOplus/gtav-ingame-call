#pragma once

#include <windows.h>

#include <string>

namespace script
{
	struct Status
	{
		bool inGame = false;
		std::string callState = "idle"; // idle | ringing | connecting | active
		std::string callId;
	};

	// Thread-safe snapshot for the API threads.
	Status status();

	void main(); // ScriptHookV script entry (runs as a fiber on the game thread)
	// Process exit: drop state whose destructors would wait on dead threads.
	void abandon();
	void onKeyboard(DWORD key, WORD repeats, BYTE scanCode, BOOL isExtended, BOOL isWithAlt, BOOL wasDownBefore, BOOL isUpNow);
}
