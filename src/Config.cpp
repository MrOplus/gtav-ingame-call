#include "Config.h"

#include <windows.h>

#include <string>

#include "Log.h"

Config g_config;

extern HMODULE g_module;

std::wstring moduleDirectory()
{
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(g_module, path, MAX_PATH);
	std::wstring dir(path);
	return dir.substr(0, dir.find_last_of(L"\\/") + 1);
}

static std::wstring iniPath()
{
	return moduleDirectory() + L"PhoneLink.ini";
}

static std::string readString(const wchar_t* section, const wchar_t* key, const std::string& def)
{
	wchar_t buf[512];
	std::wstring wdef(def.begin(), def.end());
	GetPrivateProfileStringW(section, key, wdef.c_str(), buf, 512, iniPath().c_str());
	int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
	std::string out(len > 0 ? len - 1 : 0, '\0');
	WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), len, nullptr, nullptr);
	return out;
}

static int readInt(const wchar_t* section, const wchar_t* key, int def)
{
	std::string s = readString(section, key, std::to_string(def));
	try
	{
		return std::stoi(s, nullptr, 0); // accepts 0x.. hex for key codes
	}
	catch (...)
	{
		return def;
	}
}

static float readFloat(const wchar_t* section, const wchar_t* key, float def)
{
	try
	{
		return std::stof(readString(section, key, std::to_string(def)));
	}
	catch (...)
	{
		return def;
	}
}

void loadConfig()
{
	Config& c = g_config;
	c.host = readString(L"Server", L"Host", c.host);
	c.httpPort = readInt(L"Server", L"HttpPort", c.httpPort);
	c.wsPort = readInt(L"Server", L"WsPort", c.wsPort);
	c.token = readString(L"Server", L"Token", c.token);

	c.ringTimeoutSec = readInt(L"Call", L"RingTimeoutSec", c.ringTimeoutSec);
	c.ringtone = readString(L"Call", L"Ringtone", c.ringtone);
	c.defaultIcon = readString(L"Call", L"DefaultIcon", c.defaultIcon);
	c.phoneAnimation = readInt(L"Call", L"PhoneAnimation", c.phoneAnimation) != 0;
	c.phoneAnimMode = readInt(L"Call", L"PhoneAnimMode", c.phoneAnimMode);
	c.answerKey = readInt(L"Call", L"AnswerKey", c.answerKey);
	c.hangupKey = readInt(L"Call", L"HangupKey", c.hangupKey);
	c.contactsKey = readInt(L"Call", L"ContactsKey", c.contactsKey);

	c.volume = readFloat(L"Audio", L"Volume", c.volume);
	c.ttsVoice = readString(L"Audio", L"TtsVoice", c.ttsVoice);
	c.ttsRate = readInt(L"Audio", L"TtsRate", c.ttsRate);
	c.micSampleRate = readInt(L"Audio", L"MicSampleRate", c.micSampleRate);

	c.logLevel = readString(L"Log", L"Level", c.logLevel);
}

void logConfig()
{
	const Config& c = g_config;
	LOG_INFO("config: http=%s:%d ws=%s:%d auth=%s", c.host.c_str(), c.httpPort, c.host.c_str(), c.wsPort,
		c.token.empty() ? "off" : "token");
	LOG_INFO("config: ring=%ds ringtone=%s icon=%s anim=%d mode=%d keys answer=0x%02X hangup=0x%02X contacts=0x%02X",
		c.ringTimeoutSec, c.ringtone.c_str(), c.defaultIcon.c_str(), c.phoneAnimation, c.phoneAnimMode, c.answerKey,
		c.hangupKey, c.contactsKey);
	LOG_INFO("config: volume=%.2f tts voice='%s' rate=%d mic=%dHz log=%s", c.volume, c.ttsVoice.c_str(), c.ttsRate,
		c.micSampleRate, c.logLevel.c_str());
}
