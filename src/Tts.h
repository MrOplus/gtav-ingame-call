// Offline text-to-speech with Windows SAPI, rendered to an in-memory WAV.
#pragma once

#include <cstdint>
#include <future>
#include <string>
#include <vector>

namespace tts
{
	struct Result
	{
		bool ok = false;
		std::string error;
		std::vector<uint8_t> wav;
	};

	// Runs on a worker thread; poll the future from the script fiber.
	std::future<Result> synthesizeAsync(std::string utf8Text, std::string voiceName, int rate);
}
