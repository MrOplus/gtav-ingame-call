// Audio output/input outside of the game's audio engine (miniaudio / WASAPI).
// All functions are thread-safe.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace audio
{
	bool init(float volume);
	void shutdown();

	// One-shot clip (wav/mp3/flac). Replaces any clip already playing.
	bool playClipMemory(std::vector<uint8_t> data, float* lengthSec = nullptr);
	bool playClipFile(const std::string& utf8Path, float* lengthSec = nullptr);
	bool isClipPlaying();
	void stopClip();

	// Live stream of interleaved signed 16-bit little-endian PCM.
	bool streamStart(uint32_t sampleRate, uint32_t channels);
	void streamPush(const void* pcm16, size_t bytes);
	void streamStop();

	// Microphone capture as interleaved s16 PCM. `sink` runs on the audio thread.
	using MicSink = std::function<void(const int16_t* samples, size_t sampleCount)>;
	bool micStart(uint32_t sampleRate, uint32_t channels, MicSink sink);
	void micStop();
}
