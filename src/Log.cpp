#include "Log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>

namespace logx
{
	static std::mutex g_mx;
	static FILE* g_file = nullptr;
	static std::atomic<int> g_level{static_cast<int>(Level::Info)};

	void init(const std::wstring& path)
	{
		std::lock_guard lk(g_mx);
		if (g_file)
			return;
		std::wstring prev = path.substr(0, path.find_last_of(L'.')) + L".prev.log";
		MoveFileExW(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
		g_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR); // readable while the game runs
	}

	void setLevel(Level level)
	{
		g_level = static_cast<int>(level);
	}

	Level parseLevel(const std::string& name, Level def)
	{
		std::string n = name;
		std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
		if (n == "debug") return Level::Debug;
		if (n == "info") return Level::Info;
		if (n == "warn" || n == "warning") return Level::Warn;
		if (n == "error") return Level::Error;
		return def;
	}

	bool enabled(Level level)
	{
		return static_cast<int>(level) >= g_level.load();
	}

	void write(Level level, const char* fmt, ...)
	{
		if (!enabled(level))
			return;

		char msg[2048];
		va_list args;
		va_start(args, fmt);
		vsnprintf(msg, sizeof(msg), fmt, args);
		va_end(args);

		static const char* names[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
		SYSTEMTIME t;
		GetLocalTime(&t);

		std::lock_guard lk(g_mx);
		if (!g_file)
			return;
		fprintf(g_file, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] %s [T%05lu] %s\n", t.wYear, t.wMonth, t.wDay,
			t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, names[static_cast<int>(level)], GetCurrentThreadId(), msg);
		fflush(g_file);
	}
}
