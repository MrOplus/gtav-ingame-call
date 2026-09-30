// Integration with the game's own phone (iFruit / Badger / Facade), modelled on
// iFruitAddon2: friends appear in the phone's Contacts app and outgoing calls use
// the phone's call screen. All functions must run on the script fiber.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Commands.h"

namespace phone
{
	// Call every frame. While the Contacts app is open, injects `contacts`; when the player
	// selects one of them (and `canDial`), returns its id and switches the phone to the call screen.
	std::optional<std::string> update(const std::vector<Contact>& contacts, bool canDial, const std::string& icon);

	bool isUp(); // the phone is on screen

	// Call screen (view 4) with a status label, e.g. "CELL_211" (DIALING...) or "CELL_219" (CONNECTED).
	void showCallScreen(const std::string& name, const std::string& icon, const char* statusLabel);

	// The phone's own back/hang-up button was pressed this frame.
	bool cancelPressed();

	void startRingback();
	void stopRingback();

	// Put the phone away (restarts the game's phone scripts, like iFruitAddon2).
	void close();

	// Release references (script restart / shutdown).
	void reset();
}
