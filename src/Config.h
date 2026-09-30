#pragma once

#include <string>

struct Config
{
	// [Server]
	std::string host = "127.0.0.1";
	int httpPort = 8765;
	int wsPort = 8766;
	std::string token; // empty = no auth

	// [Call]
	int ringTimeoutSec = 25;
	std::string ringtone = "Remote_Ring";
	std::string defaultIcon = "CHAR_DEFAULT";
	bool phoneAnimation = true;
	int phoneAnimMode = 0;
	int answerKey = 0x59; // Y
	int hangupKey = 0x4E; // N
	int contactsKey = 0x76; // F7: open the contacts menu to call a friend

	// [Audio]
	float volume = 1.0f;
	std::string ttsVoice; // e.g. "Microsoft Zira Desktop"; empty = system default
	int ttsRate = 0;      // -10 .. 10
	int micSampleRate = 16000;

	// [Log]
	std::string logLevel = "info"; // debug | info | warn | error
};

extern Config g_config;

std::wstring moduleDirectory();
void loadConfig();
void logConfig();
