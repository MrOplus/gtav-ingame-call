// Commands flow from the network threads to the script fiber through a
// thread-safe queue; natives may only be called from the script fiber.
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

struct SmsCommand
{
	std::string from;
	std::string subject;
	std::string text;
	std::string icon;
	bool flash = false;
};

enum class CallAudio
{
	Clip,   // encoded audio bytes (wav/mp3/flac)
	File,   // local audio file path
	Tts,    // synthesize `text` with Windows SAPI
	Stream, // live PCM frames over the owning WebSocket connection
};

struct CallCommand
{
	std::string id;
	std::string from;
	std::string icon;
	std::string text; // transcript: shown as subtitles, and spoken when audio == Tts
	int ringTimeoutSec = 0;
	CallAudio audio = CallAudio::Tts;
	std::vector<uint8_t> clip;
	std::string filePath;

	// Stream mode
	std::string ownerConnection; // WebSocket connection id that streams audio
	uint32_t sampleRate = 24000;
	uint32_t channels = 1;
	bool mic = false; // send player's microphone back to the owner
	uint32_t micSampleRate = 16000;

	// Outgoing call placed by the player: no ringing in-game; the phone shows
	// "Calling..." until a ConnectCommand arrives (the other side picked up).
	bool outgoing = false;
};

struct HangupCommand
{
	std::string id;     // empty = whatever call is active
	std::string reason; // optional: declined | no_answer | unavailable | ...
};

// The other side of an outgoing call answered.
struct ConnectCommand
{
	std::string id;
};

// Friends who can be called from the in-game contacts menu.
struct Contact
{
	std::string id;
	std::string name;
};

struct ContactsCommand
{
	std::vector<Contact> contacts;
};

// Shows a subtitle line during an active call (used by streaming clients).
struct SubtitleCommand
{
	std::string id; // empty = whatever call is active
	std::string text;
	int durationMs = 3000;
};

using Command = std::variant<SmsCommand, CallCommand, HangupCommand, SubtitleCommand, ConnectCommand, ContactsCommand>;

template <typename T>
class ThreadQueue
{
public:
	void push(T item)
	{
		std::lock_guard lk(m_mx);
		m_items.push_back(std::move(item));
	}

	std::optional<T> pop()
	{
		std::lock_guard lk(m_mx);
		if (m_items.empty())
			return std::nullopt;
		T item = std::move(m_items.front());
		m_items.pop_front();
		return item;
	}

private:
	std::mutex m_mx;
	std::deque<T> m_items;
};

extern ThreadQueue<Command> g_commands;
