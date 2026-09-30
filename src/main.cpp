#include <windows.h>

#include "main.h"

#include "ApiServer.h"
#include "Audio.h"
#include "Config.h"
#include "Log.h"
#include "Script.h"
#include "Watchdog.h"

HMODULE g_module = nullptr;

BOOL APIENTRY DllMain(HMODULE hInstance, DWORD reason, LPVOID reserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
	{
		g_module = hInstance;
		DisableThreadLibraryCalls(hInstance);
		logx::init(moduleDirectory() + L"PhoneLink.log");
		loadConfig();
		logx::setLevel(logx::parseLevel(g_config.logLevel, logx::Level::Info));

		wchar_t path[MAX_PATH];
		GetModuleFileNameW(hInstance, path, MAX_PATH);
		LOG_INFO("PhoneLink %s attached (pid %lu, module %ls, base %p)", PHONELINK_VERSION, GetCurrentProcessId(), path, hInstance);
		logConfig();

		scriptRegister(hInstance, script::main);
		keyboardHandlerRegister(script::onKeyboard);
		LOG_INFO("script registered; ScriptHookV launches it once the game world has loaded");
		watchdog::start();
		break;
	}
	case DLL_PROCESS_DETACH:
		LOG_INFO("detaching (%s)", reserved ? "process exit" : "module unload");
		scriptUnregister(hInstance);
		keyboardHandlerUnregister(script::onKeyboard);
		if (reserved)
		{
			// Process exit: every other thread has already been terminated. Normal
			// teardown would join or wait on them (std::terminate / hang), so abandon.
			watchdog::abandon();
			api::abandon();
			script::abandon();
		}
		else
		{
			// Explicit unload (ScriptHookV reload): threads are alive, shut down properly.
			watchdog::stop();
			api::stop();
			audio::shutdown();
		}
		LOG_INFO("PhoneLink unloaded");
		break;
	}
	return TRUE;
}
