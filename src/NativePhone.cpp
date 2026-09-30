#include "NativePhone.h"

#include "natives.h"

#include "Log.h"

namespace phone
{
	namespace
	{
		constexpr int kContactsView = 2;
		constexpr int kCallView = 4;
		constexpr int kFirstSlot = 50; // above the game's own contacts (iFruitAddon2 starts at 40)
		constexpr int kSelectControl = 176; // INPUT_CELLPHONE_SELECT
		constexpr int kCancelControl = 177; // INPUT_CELLPHONE_CANCEL

		int g_movie = 0;
		std::string g_movieName;
		int g_ringSound = -1;
		std::vector<Contact> g_shown; // contacts in slot order, as last injected

		// Jenkins one-at-a-time, the game's GET_HASH_KEY (lower-cased).
		uint32_t joaat(const char* s)
		{
			uint32_t h = 0;
			for (; *s; ++s)
			{
				h += static_cast<uint8_t>(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s);
				h += h << 10;
				h ^= h >> 6;
			}
			h += h << 3;
			h ^= h >> 11;
			h += h << 15;
			return h;
		}

		bool scriptRunning(const char* name)
		{
			return SCRIPT::GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH(joaat(name)) > 0;
		}

		const char* movieForPlayer()
		{
			Hash model = ENTITY::GET_ENTITY_MODEL(PLAYER::PLAYER_PED_ID());
			if (model == joaat("player_one"))
				return "cellphone_badger"; // Franklin
			if (model == joaat("player_two"))
				return "cellphone_facade"; // Trevor
			return "cellphone_ifruit";     // Michael and anyone else
		}

		void releaseMovie()
		{
			if (g_movie)
				GRAPHICS::SET_SCALEFORM_MOVIE_AS_NO_LONGER_NEEDED(&g_movie);
			g_movie = 0;
			g_movieName.clear();
		}

		// The phone scripts already have the movie loaded; requesting it returns that instance.
		bool acquireMovie()
		{
			const char* name = movieForPlayer();
			if (g_movie && g_movieName != name)
				releaseMovie();
			if (!g_movie)
			{
				g_movie = GRAPHICS::REQUEST_SCALEFORM_MOVIE(name);
				g_movieName = name;
			}
			return g_movie && GRAPHICS::HAS_SCALEFORM_MOVIE_LOADED(g_movie);
		}

		void addString(const std::string& s)
		{
			GRAPHICS::BEGIN_TEXT_COMMAND_SCALEFORM_STRING("STRING");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(s.c_str());
			GRAPHICS::END_TEXT_COMMAND_SCALEFORM_STRING();
		}

		void addLabel(const char* label)
		{
			GRAPHICS::BEGIN_TEXT_COMMAND_SCALEFORM_STRING(label);
			GRAPHICS::END_TEXT_COMMAND_SCALEFORM_STRING();
		}

		void addPicture(const std::string& txd)
		{
			GRAPHICS::BEGIN_TEXT_COMMAND_SCALEFORM_STRING("CELL_2000");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(txd.c_str());
			GRAPHICS::END_TEXT_COMMAND_SCALEFORM_STRING();
		}

		void ensureTexture(const std::string& txd)
		{
			if (!GRAPHICS::HAS_STREAMED_TEXTURE_DICT_LOADED(txd.c_str()))
				GRAPHICS::REQUEST_STREAMED_TEXTURE_DICT(txd.c_str(), FALSE);
		}

		void injectContacts(const std::vector<Contact>& contacts, const std::string& icon)
		{
			ensureTexture(icon);
			for (size_t i = 0; i < contacts.size(); ++i)
			{
				GRAPHICS::BEGIN_SCALEFORM_MOVIE_METHOD(g_movie, "SET_DATA_SLOT");
				GRAPHICS::SCALEFORM_MOVIE_METHOD_ADD_PARAM_INT(kContactsView);
				GRAPHICS::SCALEFORM_MOVIE_METHOD_ADD_PARAM_INT(kFirstSlot + static_cast<int>(i));
				GRAPHICS::SCALEFORM_MOVIE_METHOD_ADD_PARAM_INT(0);
				addString(contacts[i].name);
				addLabel("CELL_999");
				addPicture(icon);
				GRAPHICS::END_SCALEFORM_MOVIE_METHOD();
			}
		}

		// Ask the phone which row is highlighted. The result arrives a frame or two later.
		int currentSelection()
		{
			GRAPHICS::BEGIN_SCALEFORM_MOVIE_METHOD(g_movie, "GET_CURRENT_SELECTION");
			int ret = GRAPHICS::END_SCALEFORM_MOVIE_METHOD_RETURN_VALUE();
			for (int i = 0; i < 30 && !GRAPHICS::IS_SCALEFORM_MOVIE_METHOD_RETURN_VALUE_READY(ret); ++i)
				WAIT(0);
			if (!GRAPHICS::IS_SCALEFORM_MOVIE_METHOD_RETURN_VALUE_READY(ret))
				return -1;
			return GRAPHICS::GET_SCALEFORM_MOVIE_METHOD_RETURN_VALUE_INT(ret);
		}

		// Selecting an unknown slot makes the game post "contact not available"; drop it.
		void removeLatestFeedItem()
		{
			HUD::BEGIN_TEXT_COMMAND_THEFEED_POST("STRING");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME("");
			int id = HUD::END_TEXT_COMMAND_THEFEED_POST_TICKER(FALSE, FALSE);
			HUD::THEFEED_REMOVE_ITEM(id);
			HUD::THEFEED_REMOVE_ITEM(id - 1);
		}

		bool selectPressed()
		{
			return PAD::IS_CONTROL_JUST_PRESSED(0, kSelectControl) || PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, kSelectControl);
		}

		void restartScript(const char* name)
		{
			SCRIPT::REQUEST_SCRIPT(name);
			for (int i = 0; i < 200 && !SCRIPT::HAS_SCRIPT_LOADED(name); ++i)
				WAIT(0);
			if (SCRIPT::HAS_SCRIPT_LOADED(name))
			{
				BUILTIN::START_NEW_SCRIPT(name, 1424);
				SCRIPT::SET_SCRIPT_AS_NO_LONGER_NEEDED(name);
			}
			else
				LOG_WARN("phone: could not reload script %s", name);
		}
	}

	bool isUp()
	{
		return scriptRunning("cellphone_flashhand");
	}

	std::optional<std::string> update(const std::vector<Contact>& contacts, bool canDial, const std::string& icon)
	{
		if (!isUp())
		{
			releaseMovie();
			g_shown.clear();
			return std::nullopt;
		}
		if (!scriptRunning("appcontacts") || !acquireMovie())
			return std::nullopt;

		// The contacts app redraws its list; re-inject every frame so our entries stay.
		g_shown = contacts;
		injectContacts(g_shown, icon);

		if (!canDial || g_shown.empty() || !selectPressed())
			return std::nullopt;
		int sel = currentSelection();
		int idx = sel - kFirstSlot;
		if (idx < 0 || idx >= static_cast<int>(g_shown.size()))
			return std::nullopt; // one of the game's own contacts: let the game handle it

		const Contact c = g_shown[idx];
		MISC::TERMINATE_ALL_SCRIPTS_WITH_THIS_NAME("appcontacts"); // the game must not handle this call
		showCallScreen(c.name, icon, "CELL_211");                  // DIALING...
		WAIT(10);
		removeLatestFeedItem();
		LOG("phone: player selected contact '%s' (slot %d)", c.name.c_str(), sel);
		return c.id;
	}

	void showCallScreen(const std::string& name, const std::string& icon, const char* statusLabel)
	{
		if (!isUp() || !acquireMovie())
			return;
		ensureTexture(icon);
		GRAPHICS::BEGIN_SCALEFORM_MOVIE_METHOD(g_movie, "SET_DATA_SLOT");
		GRAPHICS::SCALEFORM_MOVIE_METHOD_ADD_PARAM_INT(kCallView);
		GRAPHICS::SCALEFORM_MOVIE_METHOD_ADD_PARAM_INT(0);
		GRAPHICS::SCALEFORM_MOVIE_METHOD_ADD_PARAM_INT(3);
		GRAPHICS::BEGIN_TEXT_COMMAND_SCALEFORM_STRING("STRING");
		HUD::ADD_TEXT_COMPONENT_SUBSTRING_PHONE_NUMBER(name.c_str(), -1);
		GRAPHICS::END_TEXT_COMMAND_SCALEFORM_STRING();
		addPicture(icon);
		addLabel(statusLabel);
		GRAPHICS::END_SCALEFORM_MOVIE_METHOD();

		GRAPHICS::BEGIN_SCALEFORM_MOVIE_METHOD(g_movie, "DISPLAY_VIEW");
		GRAPHICS::SCALEFORM_MOVIE_METHOD_ADD_PARAM_INT(kCallView);
		GRAPHICS::END_SCALEFORM_MOVIE_METHOD();
	}

	bool cancelPressed()
	{
		return isUp() && (PAD::IS_CONTROL_JUST_PRESSED(0, kCancelControl) ||
			PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, kCancelControl));
	}

	void startRingback()
	{
		stopRingback();
		g_ringSound = AUDIO::GET_SOUND_ID();
		AUDIO::PLAY_SOUND_FRONTEND(g_ringSound, "Dial_and_Remote_Ring", "Phone_SoundSet_Default", TRUE);
	}

	void stopRingback()
	{
		if (g_ringSound != -1)
		{
			AUDIO::STOP_SOUND(g_ringSound);
			AUDIO::RELEASE_SOUND_ID(g_ringSound);
			g_ringSound = -1;
		}
	}

	void close()
	{
		stopRingback();
		if (!isUp())
		{
			releaseMovie();
			return;
		}
		if (acquireMovie())
		{
			GRAPHICS::BEGIN_SCALEFORM_MOVIE_METHOD(g_movie, "SHUTDOWN_MOVIE");
			GRAPHICS::END_SCALEFORM_MOVIE_METHOD();
		}
		WAIT(0);
		GTA::DESTROY_MOBILE_PHONE();
		MISC::TERMINATE_ALL_SCRIPTS_WITH_THIS_NAME("cellphone_flashhand");
		MISC::TERMINATE_ALL_SCRIPTS_WITH_THIS_NAME("cellphone_controller");
		releaseMovie();
		WAIT(0);
		restartScript("cellphone_flashhand");
		restartScript("cellphone_controller");
		LOG("phone: closed");
	}

	void reset()
	{
		stopRingback();
		releaseMovie();
		g_shown.clear();
	}
}
