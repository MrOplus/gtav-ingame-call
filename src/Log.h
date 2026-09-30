#pragma once

#include <string>

namespace logx
{
	enum class Level
	{
		Debug = 0,
		Info = 1,
		Warn = 2,
		Error = 3,
	};

	// Opens <path>, keeping the previous session's log as <name>.prev.log.
	void init(const std::wstring& path);
	void setLevel(Level level);
	Level parseLevel(const std::string& name, Level def);
	bool enabled(Level level);
	void write(Level level, const char* fmt, ...);
}

#define LOG_DEBUG(...) do { if (::logx::enabled(::logx::Level::Debug)) ::logx::write(::logx::Level::Debug, __VA_ARGS__); } while (0)
#define LOG_INFO(...) ::logx::write(::logx::Level::Info, __VA_ARGS__)
#define LOG_WARN(...) ::logx::write(::logx::Level::Warn, __VA_ARGS__)
#define LOG_ERROR(...) ::logx::write(::logx::Level::Error, __VA_ARGS__)
#define LOG(...) LOG_INFO(__VA_ARGS__)
