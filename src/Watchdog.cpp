#include "Watchdog.h"

#include <windows.h>

#include <atomic>
#include <thread>

#include "Log.h"

namespace watchdog
{
	namespace
	{
		std::atomic<uint64_t> g_lastTick{0};
		std::atomic<bool> g_running{false};
		std::thread g_thread;
		HANDLE g_stopEvent = nullptr;

		constexpr uint64_t kStallMs = 10'000;
		constexpr uint64_t kWaitReportMs = 30'000;

		void run()
		{
			const uint64_t loadedAt = GetTickCount64();
			uint64_t nextWaitReport = loadedAt + kWaitReportMs;
			bool stalled = false;
			uint64_t stalledSince = 0;

			while (WaitForSingleObject(g_stopEvent, 1000) == WAIT_TIMEOUT)
			{
				uint64_t now = GetTickCount64();
				uint64_t last = g_lastTick.load();
				if (last == 0)
				{
					if (now >= nextWaitReport)
					{
						LOG_INFO("watchdog: still waiting for ScriptHookV to launch the script (%llus since load)",
							(now - loadedAt) / 1000);
						nextWaitReport = now + kWaitReportMs;
					}
					continue;
				}
				if (!stalled && now - last > kStallMs)
				{
					stalled = true;
					stalledSince = last;
					LOG_WARN("watchdog: script has not ticked for %llus (loading screen, pause menu, minimized, or game hung)",
						(now - last) / 1000);
				}
				else if (stalled && now - last < kStallMs)
				{
					stalled = false;
					LOG_INFO("watchdog: script resumed after %llus", (last - stalledSince) / 1000);
				}
			}
		}
	}

	void start()
	{
		if (g_running.exchange(true))
			return;
		g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		g_thread = std::thread(run);
	}

	void stop()
	{
		if (!g_running.exchange(false))
			return;
		SetEvent(g_stopEvent);
		if (g_thread.joinable())
			g_thread.join();
		CloseHandle(g_stopEvent);
	}

	void abandon()
	{
		if (g_thread.joinable())
			g_thread.detach(); // destroying a joinable std::thread calls std::terminate
		g_running = false;
	}

	void tick()
	{
		uint64_t prev = g_lastTick.exchange(GetTickCount64());
		if (prev == 0)
			LOG_INFO("watchdog: first script tick");
	}
}
