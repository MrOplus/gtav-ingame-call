#include "Script.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <optional>
#include <variant>

#include "natives.h"

#include "json.hpp"

#include "ApiServer.h"
#include "Audio.h"
#include "Commands.h"
#include "Config.h"
#include "Log.h"
#include "Tts.h"
#include "Watchdog.h"

using json = nlohmann::json;

ThreadQueue<Command> g_commands;

namespace script
{
	namespace
	{
		// ---- shared state ----------------------------------------------------
		std::mutex g_statusMx;
		Status g_status;

		std::atomic<bool> g_answerPressed{false};
		std::atomic<bool> g_hangupPressed{false};

		// Contacts menu input (set by the keyboard handler, consumed by the fiber).
		std::atomic<bool> g_menuOpen{false};
		std::atomic<bool> g_menuKeyPressed{false};
		std::atomic<int> g_menuMove{0}; // -1 up, +1 down (accumulated)
		std::atomic<bool> g_menuSelect{false};
		std::atomic<bool> g_menuClose{false};

		enum class State
		{
			Idle,
			Ringing,
			Dialing,    // outgoing: waiting for the other side to pick up
			Connecting, // answered, waiting for TTS
			Active,
		};

		const char* stateName(State s)
		{
			switch (s)
			{
			case State::Ringing: return "ringing";
			case State::Dialing: return "dialing";
			case State::Connecting: return "connecting";
			case State::Active: return "active";
			default: return "idle";
			}
		}

		struct Subtitle
		{
			uint64_t startMs;
			int durationMs;
			std::string text;
		};

		struct Call
		{
			CallCommand cmd;
			uint64_t ringStartMs = 0;
			uint64_t lastRingMs = 0;
			uint64_t answeredMs = 0;
			std::optional<std::future<tts::Result>> tts;
			bool animStarted = false;
			std::vector<Subtitle> subtitles;
			size_t nextSubtitle = 0;
		};

		State g_state = State::Idle;
		std::optional<Call> g_call;

		std::vector<Contact> g_contacts;
		int g_menuSel = 0;
		constexpr uint64_t kDialTimeoutMs = 45'000; // safety net; Studio normally ends it first

		uint64_t nowMs()
		{
			return GetTickCount64();
		}

		void publishStatus(bool inGame)
		{
			std::lock_guard lk(g_statusMx);
			g_status.inGame = inGame;
			g_status.callState = stateName(g_state);
			g_status.callId = g_call ? g_call->cmd.id : std::string();
		}

		void emit(const char* event, const std::string& callId, json extra = json::object())
		{
			extra["event"] = event;
			if (!callId.empty())
				extra["id"] = callId;
			api::broadcast(extra.dump());
		}

		std::string keyName(int vk)
		{
			if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
				return std::string(1, static_cast<char>(vk));
			if (vk >= VK_F1 && vk <= VK_F24)
				return "F" + std::to_string(vk - VK_F1 + 1);
			char buf[16];
			snprintf(buf, sizeof(buf), "0x%02X", vk);
			return buf;
		}

		// ---- text helpers ----------------------------------------------------
		// Text components are limited to 99 bytes; split long UTF-8 strings.
		void addLongString(const std::string& s)
		{
			size_t pos = 0;
			while (pos < s.size())
			{
				size_t len = std::min<size_t>(99, s.size() - pos);
				while (len > 0 && pos + len < s.size() && (static_cast<unsigned char>(s[pos + len]) & 0xC0) == 0x80)
					--len; // don't split a UTF-8 sequence
				HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(s.substr(pos, len).c_str());
				pos += len;
			}
		}

		void feedMessage(const std::string& icon, const std::string& sender, const std::string& subject,
			const std::string& text, bool flash)
		{
			HUD::BEGIN_TEXT_COMMAND_THEFEED_POST("STRING");
			addLongString(text);
			HUD::END_TEXT_COMMAND_THEFEED_POST_MESSAGETEXT(icon.c_str(), icon.c_str(), flash, 1, sender.c_str(), subject.c_str());
			HUD::END_TEXT_COMMAND_THEFEED_POST_TICKER(false, true);
		}

		void drawText(const std::string& s, float x, float y, float scale, int font, int r, int g, int b, int a, bool center)
		{
			HUD::SET_TEXT_FONT(font);
			HUD::SET_TEXT_SCALE(0.0f, scale);
			HUD::SET_TEXT_COLOUR(r, g, b, a);
			HUD::SET_TEXT_CENTRE(center);
			HUD::SET_TEXT_OUTLINE();
			HUD::BEGIN_TEXT_COMMAND_DISPLAY_TEXT("STRING");
			addLongString(s);
			HUD::END_TEXT_COMMAND_DISPLAY_TEXT(x, y, 0);
		}

		void showSubtitle(const std::string& text, int durationMs)
		{
			HUD::BEGIN_TEXT_COMMAND_PRINT("STRING");
			addLongString(text);
			HUD::END_TEXT_COMMAND_PRINT(durationMs, true);
		}

		// Split a transcript into sentences, timed proportionally to their length.
		std::vector<Subtitle> buildSubtitles(const std::string& text, float lengthSec, uint64_t startMs)
		{
			std::vector<std::string> parts;
			std::string cur;
			for (size_t i = 0; i < text.size(); ++i)
			{
				cur += text[i];
				bool end = (text[i] == '.' || text[i] == '!' || text[i] == '?') &&
					(i + 1 == text.size() || text[i + 1] == ' ');
				if (end || i + 1 == text.size())
				{
					size_t b = cur.find_first_not_of(' ');
					if (b != std::string::npos)
						parts.push_back(cur.substr(b));
					cur.clear();
				}
			}
			size_t totalChars = 0;
			for (auto& p : parts)
				totalChars += p.size();
			if (totalChars == 0)
				return {};

			double totalMs = lengthSec > 0.1f ? lengthSec * 1000.0 : totalChars * 70.0;
			std::vector<Subtitle> subs;
			double t = 0;
			for (auto& p : parts)
			{
				double d = totalMs * p.size() / totalChars;
				subs.push_back({startMs + static_cast<uint64_t>(t), static_cast<int>(d) + 300, p});
				t += d;
			}
			return subs;
		}

		// ---- call flow -------------------------------------------------------
		void endCall(const char* reason)
		{
			if (!g_call)
				return;
			Ped ped = PLAYER::PLAYER_PED_ID();
			AUDIO::STOP_PED_RINGTONE(ped);
			audio::stopClip();
			audio::streamStop();
			audio::micStop();
			api::clearStreamOwner();
			if (g_call->animStarted)
				TASK::TASK_USE_MOBILE_PHONE(ped, FALSE, g_config.phoneAnimMode);
			if (g_state == State::Active || g_state == State::Connecting || g_state == State::Dialing)
				AUDIO::PLAY_SOUND_FRONTEND(-1, "Hang_Up", "Phone_SoundSet_Michael", true);

			json extra = {{"reason", reason}};
			if (g_call->answeredMs)
				extra["duration_ms"] = nowMs() - g_call->answeredMs;
			LOG("call %s ended: %s", g_call->cmd.id.c_str(), reason);
			emit("call_ended", g_call->cmd.id, extra);

			g_call.reset();
			g_state = State::Idle;
		}

		bool startMedia(Call& call)
		{
			float lengthSec = 0.0f;
			bool ok = false;
			switch (call.cmd.audio)
			{
			case CallAudio::Clip:
				ok = audio::playClipMemory(std::move(call.cmd.clip), &lengthSec);
				break;
			case CallAudio::File:
				ok = audio::playClipFile(call.cmd.filePath, &lengthSec);
				break;
			case CallAudio::Tts:
			{
				tts::Result r = call.tts->get();
				call.tts.reset();
				if (!r.ok)
				{
					LOG_ERROR("tts failed: %s", r.error.c_str());
					return false;
				}
				LOG_INFO("tts ready: %zu bytes of audio", r.wav.size());
				ok = audio::playClipMemory(std::move(r.wav), &lengthSec);
				break;
			}
			case CallAudio::Stream:
				ok = audio::streamStart(call.cmd.sampleRate, call.cmd.channels);
				if (ok && call.cmd.mic)
				{
					audio::micStart(call.cmd.micSampleRate, 1, [](const int16_t* s, size_t n) {
						api::sendBinaryToOwner(s, n * sizeof(int16_t));
					});
				}
				break;
			}
			if (ok && !call.cmd.text.empty() && call.cmd.audio != CallAudio::Stream)
				call.subtitles = buildSubtitles(call.cmd.text, lengthSec, nowMs());
			if (ok && call.cmd.audio != CallAudio::Stream)
				LOG_INFO("clip playing: %.1fs, %zu subtitle line(s)", lengthSec, call.subtitles.size());
			return ok;
		}

		void activate()
		{
			Call& call = *g_call;
			LOG_INFO("call %s starting media (%s)", call.cmd.id.c_str(),
				call.cmd.audio == CallAudio::Stream ? "stream" : call.cmd.audio == CallAudio::Tts ? "tts" :
				call.cmd.audio == CallAudio::Clip ? "clip" : "file");
			if (!startMedia(call))
			{
				LOG_ERROR("call %s: could not start audio", call.cmd.id.c_str());
				endCall("media_failed");
				return;
			}
			g_state = State::Active;
			json extra;
			if (call.cmd.audio == CallAudio::Stream)
				extra = {{"sample_rate", call.cmd.sampleRate}, {"channels", call.cmd.channels},
					{"mic", call.cmd.mic}, {"mic_sample_rate", call.cmd.micSampleRate}};
			emit("call_active", call.cmd.id, extra);
		}

		void answer()
		{
			Call& call = *g_call;
			Ped ped = PLAYER::PLAYER_PED_ID();
			AUDIO::STOP_PED_RINGTONE(ped);
			call.answeredMs = nowMs();
			if (g_config.phoneAnimation)
			{
				TASK::TASK_USE_MOBILE_PHONE(ped, TRUE, g_config.phoneAnimMode);
				call.animStarted = true;
			}
			LOG("call %s answered", call.cmd.id.c_str());
			emit("call_answered", call.cmd.id);

			bool ttsPending = call.tts && call.tts->wait_for(std::chrono::seconds(0)) != std::future_status::ready;
			if (ttsPending)
				g_state = State::Connecting;
			else
				activate();
		}

		void startCall(CallCommand cmd)
		{
			if (g_state != State::Idle)
			{
				LOG_WARN("call %s rejected: already in call %s", cmd.id.c_str(), g_call->cmd.id.c_str());
				emit("call_rejected", cmd.id, {{"reason", "busy"}});
				return;
			}
			Call call;
			call.cmd = std::move(cmd);
			call.ringStartMs = call.lastRingMs = nowMs();
			if (call.cmd.audio == CallAudio::Tts)
				call.tts = tts::synthesizeAsync(call.cmd.text, g_config.ttsVoice, g_config.ttsRate);
			if (call.cmd.audio == CallAudio::Stream)
				api::setStreamOwner(call.cmd.ownerConnection);

			Ped ped = PLAYER::PLAYER_PED_ID();
			g_answerPressed = false;
			g_hangupPressed = false;

			if (call.cmd.outgoing)
			{
				// The player is calling out: phone to the ear, ringback until they pick up.
				AUDIO::PLAY_PED_RINGTONE("Dial_and_Remote_Ring", ped, TRUE);
				if (g_config.phoneAnimation)
				{
					TASK::TASK_USE_MOBILE_PHONE(ped, TRUE, g_config.phoneAnimMode);
					call.animStarted = true;
				}
				g_call = std::move(call);
				g_state = State::Dialing;
				LOG("call %s dialing '%s'", g_call->cmd.id.c_str(), g_call->cmd.from.c_str());
				emit("call_dialing", g_call->cmd.id, {{"to", g_call->cmd.from}});
				return;
			}

			AUDIO::PLAY_PED_RINGTONE(g_config.ringtone.c_str(), ped, TRUE);
			feedMessage(call.cmd.icon, call.cmd.from, "Incoming call",
				"[" + keyName(g_config.answerKey) + "] answer   [" + keyName(g_config.hangupKey) + "] decline", false);

			g_call = std::move(call);
			g_state = State::Ringing;
			LOG("call %s ringing from '%s'", g_call->cmd.id.c_str(), g_call->cmd.from.c_str());
			emit("call_ringing", g_call->cmd.id, {{"from", g_call->cmd.from}});
		}

		void showSms(const SmsCommand& c)
		{
			feedMessage(c.icon, c.from, c.subject.empty() ? "New message" : c.subject, c.text, c.flash);
			AUDIO::PLAY_SOUND_FRONTEND(-1, "Text_Arrive_Tone", "Phone_SoundSet_Default", true);
			LOG("sms from '%s' shown", c.from.c_str());
			emit("sms_shown", {}, {{"from", c.from}});
		}

		void drawCallPanel()
		{
			const Call& call = *g_call;
			const float x = 0.88f, y = 0.30f, w = 0.20f, h = 0.13f;
			GRAPHICS::DRAW_RECT(x, y, w, h, 10, 10, 14, 200, FALSE);
			GRAPHICS::DRAW_RECT(x, y - h / 2 + 0.002f, w, 0.004f, 93, 182, 229, 255, FALSE);

			std::string title, hint;
			switch (g_state)
			{
			case State::Ringing:
				title = "INCOMING CALL";
				hint = "[" + keyName(g_config.answerKey) + "] Answer    [" + keyName(g_config.hangupKey) + "] Decline";
				break;
			case State::Connecting:
				title = "CONNECTING...";
				hint = "[" + keyName(g_config.hangupKey) + "] Hang up";
				break;
			case State::Dialing:
				title = "CALLING...";
				hint = "[" + keyName(g_config.hangupKey) + "] Cancel";
				break;
			default:
			{
				uint64_t secs = (nowMs() - call.answeredMs) / 1000;
				char buf[32];
				snprintf(buf, sizeof(buf), "ON CALL  %02llu:%02llu", secs / 60, secs % 60);
				title = buf;
				hint = "[" + keyName(g_config.hangupKey) + "] Hang up";
				break;
			}
			}
			drawText(title, x, y - 0.055f, 0.32f, 4, 93, 182, 229, 255, true);
			drawText(call.cmd.from, x, y - 0.030f, 0.55f, 4, 255, 255, 255, 255, true);
			drawText(hint, x, y + 0.025f, 0.30f, 0, 200, 200, 200, 255, true);
		}

		void updateCall(bool playerOk)
		{
			if (!g_call)
				return;
			if (!playerOk)
			{
				endCall("player_unavailable");
				return;
			}
			Call& call = *g_call;
			bool answerKey = g_answerPressed.exchange(false);
			bool hangupKey = g_hangupPressed.exchange(false);
			uint64_t now = nowMs();

			switch (g_state)
			{
			case State::Ringing:
				if (answerKey)
				{
					answer();
					break;
				}
				if (hangupKey)
				{
					AUDIO::STOP_PED_RINGTONE(PLAYER::PLAYER_PED_ID());
					emit("call_declined", call.cmd.id);
					endCall("declined");
					break;
				}
				if (now - call.ringStartMs > static_cast<uint64_t>(call.cmd.ringTimeoutSec) * 1000)
				{
					feedMessage(call.cmd.icon, call.cmd.from, "Missed call", "You missed a call.", false);
					emit("call_missed", call.cmd.id);
					endCall("missed");
					break;
				}
				// Some ringtones are one-shot; keep it ringing.
				if (now - call.lastRingMs > 1000 && !AUDIO::IS_PED_RINGTONE_PLAYING(PLAYER::PLAYER_PED_ID()))
				{
					AUDIO::PLAY_PED_RINGTONE(g_config.ringtone.c_str(), PLAYER::PLAYER_PED_ID(), TRUE);
					call.lastRingMs = now;
				}
				break;

			case State::Dialing:
				if (hangupKey)
				{
					endCall("player_hung_up");
					break;
				}
				if (now - call.ringStartMs > kDialTimeoutMs)
				{
					feedMessage(call.cmd.icon, call.cmd.from, "No answer", call.cmd.from + " did not pick up.", false);
					endCall("no_answer");
					break;
				}
				if (now - call.lastRingMs > 1000 && !AUDIO::IS_PED_RINGTONE_PLAYING(PLAYER::PLAYER_PED_ID()))
				{
					AUDIO::PLAY_PED_RINGTONE("Dial_and_Remote_Ring", PLAYER::PLAYER_PED_ID(), TRUE);
					call.lastRingMs = now;
				}
				break;

			case State::Connecting:
				if (hangupKey)
				{
					endCall("player_hung_up");
					break;
				}
				if (call.tts && call.tts->wait_for(std::chrono::seconds(0)) == std::future_status::ready)
					activate();
				break;

			case State::Active:
				if (hangupKey)
				{
					endCall("player_hung_up");
					break;
				}
				while (call.nextSubtitle < call.subtitles.size() && call.subtitles[call.nextSubtitle].startMs <= now)
				{
					const Subtitle& s = call.subtitles[call.nextSubtitle++];
					showSubtitle(s.text, s.durationMs);
				}
				if (call.cmd.audio != CallAudio::Stream && !audio::isClipPlaying())
					endCall("completed");
				break;

			default:
				break;
			}

			if (g_call)
				drawCallPanel();
		}

		// ---- contacts menu (call a friend) ----------------------------------------
		void updateMenu(bool inGame)
		{
			if (g_menuKeyPressed.exchange(false))
			{
				if (g_menuOpen)
					g_menuOpen = false;
				else if (inGame && g_state == State::Idle)
				{
					g_menuOpen = true;
					g_menuSel = 0;
					g_menuMove = 0;
					g_menuSelect = false;
					g_menuClose = false;
					AUDIO::PLAY_SOUND_FRONTEND(-1, "Menu_Navigate", "Phone_SoundSet_Default", true);
				}
			}
			if (!g_menuOpen)
				return;
			if (!inGame || g_state != State::Idle || g_menuClose.exchange(false))
			{
				g_menuOpen = false;
				return;
			}

			PAD::DISABLE_ALL_CONTROL_ACTIONS(0); // arrows/Enter would otherwise open the phone, weapon wheel...
			int n = static_cast<int>(g_contacts.size());
			if (int move = g_menuMove.exchange(0); move != 0 && n > 0)
			{
				g_menuSel = ((g_menuSel + move) % n + n) % n;
				AUDIO::PLAY_SOUND_FRONTEND(-1, "Menu_Navigate", "Phone_SoundSet_Default", true);
			}
			if (g_menuSel >= n)
				g_menuSel = n > 0 ? n - 1 : 0;
			if (g_menuSelect.exchange(false) && n > 0)
			{
				const Contact& c = g_contacts[g_menuSel];
				LOG("player dials '%s' (%s)", c.name.c_str(), c.id.c_str());
				AUDIO::PLAY_SOUND_FRONTEND(-1, "Menu_Accept", "Phone_SoundSet_Default", true);
				emit("dial", {}, {{"contact", c.id}, {"name", c.name}});
				g_menuOpen = false;
				return;
			}

			const float x = 0.16f, w = 0.24f, rowH = 0.034f, top = 0.25f;
			const int rows = std::max(n, 1);
			const float h = 0.07f + rowH * rows + 0.035f;
			GRAPHICS::DRAW_RECT(x, top + h / 2, w, h, 10, 10, 14, 215, FALSE);
			GRAPHICS::DRAW_RECT(x, top + 0.002f, w, 0.004f, 93, 182, 229, 255, FALSE);
			drawText("CONTACTS", x, top + 0.012f, 0.40f, 4, 93, 182, 229, 255, true);
			float y = top + 0.06f;
			if (n == 0)
			{
				drawText("No friends online", x, y, 0.32f, 0, 200, 200, 200, 255, true);
				y += rowH;
			}
			for (int i = 0; i < n; ++i, y += rowH)
			{
				if (i == g_menuSel)
					GRAPHICS::DRAW_RECT(x, y + rowH / 2 - 0.004f, w - 0.01f, rowH - 0.004f, 47, 143, 196, 230, FALSE);
				drawText(g_contacts[i].name, x, y, 0.36f, 0, 255, 255, 255, 255, true);
			}
			drawText("[Up/Down] Select   [Enter] Call   [" + keyName(g_config.contactsKey) + "] Close", x, y + 0.006f,
				0.26f, 0, 170, 170, 170, 255, true);
		}

		void processCommands()
		{
			while (auto cmd = g_commands.pop())
			{
				LOG_DEBUG("processing command #%zu", cmd->index());
				std::visit([](auto&& c) {
					using T = std::decay_t<decltype(c)>;
					if constexpr (std::is_same_v<T, SmsCommand>)
						showSms(c);
					else if constexpr (std::is_same_v<T, CallCommand>)
						startCall(std::move(c));
					else if constexpr (std::is_same_v<T, HangupCommand>)
					{
						if (g_call && (c.id.empty() || c.id == g_call->cmd.id))
						{
							if (g_state == State::Dialing && !c.reason.empty())
							{
								const std::string& who = g_call->cmd.from;
								std::string text = c.reason == "declined" ? who + " declined the call."
									: c.reason == "no_answer" ? who + " did not pick up."
									: who + " is not available right now.";
								feedMessage(g_call->cmd.icon, who, "Call failed", text, false);
							}
							endCall(c.reason.empty() ? "remote_hung_up" : c.reason.c_str());
						}
					}
					else if constexpr (std::is_same_v<T, ConnectCommand>)
					{
						if (g_call && g_state == State::Dialing && c.id == g_call->cmd.id)
						{
							AUDIO::STOP_PED_RINGTONE(PLAYER::PLAYER_PED_ID());
							g_call->answeredMs = nowMs();
							LOG("call %s connected", c.id.c_str());
							activate();
						}
					}
					else if constexpr (std::is_same_v<T, ContactsCommand>)
					{
						g_contacts = c.contacts;
						LOG_DEBUG("contacts updated: %zu online", g_contacts.size());
					}
					else if constexpr (std::is_same_v<T, SubtitleCommand>)
					{
						if (g_call && g_state == State::Active && (c.id.empty() || c.id == g_call->cmd.id))
							showSubtitle(c.text, c.durationMs);
						else
							LOG_DEBUG("subtitle ignored: no active call");
					}
				}, *cmd);
			}
		}

		void initOnce()
		{
			static bool done = false;
			if (done)
				return;
			done = true;
			LOG_INFO("script fiber started (game version id %d, reload mode %d)", static_cast<int>(getGameVersion()),
				scriptsAreLaunchedUsingReloading());
			uint64_t t0 = nowMs();
			if (!audio::init(g_config.volume))
				LOG_ERROR("audio init failed - calls will ring but have no sound");
			LOG_INFO("audio init took %llums", nowMs() - t0);
			t0 = nowMs();
			if (!api::start())
				LOG_ERROR("API failed to start - is another program using the ports?");
			LOG_INFO("API init took %llums", nowMs() - t0);
		}
	}

	void abandon()
	{
		// A pending std::async future blocks in its destructor until the task finishes;
		// the task's thread is gone at process exit, so move it somewhere never destroyed.
		if (g_call && g_call->tts)
			new std::future<tts::Result>(std::move(*g_call->tts));
		g_call.reset();
	}

	Status status()
	{
		std::lock_guard lk(g_statusMx);
		return g_status;
	}

	void onKeyboard(DWORD key, WORD, BYTE, BOOL, BOOL, BOOL wasDownBefore, BOOL isUpNow)
	{
		if (wasDownBefore || isUpNow)
			return;
		if (static_cast<int>(key) == g_config.contactsKey)
		{
			g_menuKeyPressed = true;
			return;
		}
		if (g_menuOpen)
		{
			switch (key)
			{
			case VK_UP: g_menuMove -= 1; return;
			case VK_DOWN: g_menuMove += 1; return;
			case VK_RETURN: g_menuSelect = true; return;
			case VK_BACK: g_menuClose = true; return;
			default: break;
			}
		}
		if (static_cast<int>(key) == g_config.answerKey)
		{
			LOG_DEBUG("answer key pressed");
			g_answerPressed = true;
		}
		else if (static_cast<int>(key) == g_config.hangupKey)
		{
			LOG_DEBUG("hang-up key pressed");
			g_hangupPressed = true;
		}
	}

	void main()
	{
		initOnce();
		// The script fiber restarts when the game reloads scripts; drop any stale call.
		g_call.reset();
		g_state = State::Idle;

		bool wasInGame = false;
		uint64_t frames = 0, lastBeat = nowMs();
		while (true)
		{
			watchdog::tick();
			++frames;
			Player player = PLAYER::PLAYER_ID();
			Ped ped = PLAYER::PLAYER_PED_ID();
			bool inGame = PLAYER::IS_PLAYER_PLAYING(player) && !DLC::GET_IS_LOADING_SCREEN_ACTIVE();
			bool playerOk = inGame && !ENTITY::IS_ENTITY_DEAD(ped, FALSE);

			try
			{
				if (inGame)
					processCommands(); // otherwise keep them queued until the player is back
				updateCall(playerOk);
				updateMenu(playerOk);
			}
			catch (const std::exception& e)
			{
				// Never let an exception escape into the game; drop the call and carry on.
				LOG_ERROR("script: %s", e.what());
				g_call.reset();
				g_state = State::Idle;
			}
			if (g_state == State::Idle)
			{
				g_answerPressed = false;
				g_hangupPressed = false;
			}
			if (inGame != wasInGame)
			{
				LOG_INFO("player %s", inGame ? "in game - accepting commands" : "not in game (loading/cutscene) - commands are queued");
				wasInGame = inGame;
			}
			if (uint64_t now = nowMs(); now - lastBeat >= 60'000)
			{
				LOG_DEBUG("heartbeat: %.1f fps, in_game=%d, call=%s", frames * 1000.0 / (now - lastBeat), inGame, stateName(g_state));
				frames = 0;
				lastBeat = now;
			}
			publishStatus(inGame);
			WAIT(0);
		}
	}
}
