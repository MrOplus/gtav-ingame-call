// Background thread that logs when the script fiber is not running: while
// waiting for ScriptHookV to launch it, and when the game stops ticking it
// (loading screens, pause, alt-tab, or a hang).
#pragma once

namespace watchdog
{
	void start();
	void stop();
	// Process exit: the thread is already dead; forget it without joining.
	void abandon();

	// Called by the script fiber every frame.
	void tick();
}
