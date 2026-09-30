#include "Audio.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_GENERATION
#include "miniaudio.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <mutex>

#include "Log.h"

namespace audio
{
	namespace
	{
		ma_engine g_engine;
		bool g_engineOk = false;

		// ---- one-shot clips --------------------------------------------------
		struct Clip
		{
			std::vector<uint8_t> data; // must outlive the decoder
			ma_decoder decoder{};
			ma_sound sound{};
			bool hasDecoder = false;
		};

		std::mutex g_clipMx;
		std::unique_ptr<Clip> g_clip;

		void destroyClipLocked()
		{
			if (!g_clip)
				return;
			ma_sound_uninit(&g_clip->sound);
			if (g_clip->hasDecoder)
				ma_decoder_uninit(&g_clip->decoder);
			g_clip.reset();
		}

		float clipLength(ma_sound* sound)
		{
			float len = 0.0f;
			ma_sound_get_length_in_seconds(sound, &len);
			return len;
		}

		// ---- live stream -----------------------------------------------------
		// A data source that plays PCM pushed from the network. It never reaches
		// "end": on underrun it outputs silence and re-buffers (a simple jitter buffer).
		struct StreamSource
		{
			ma_data_source_base base;
			uint32_t sampleRate = 24000;
			uint32_t channels = 1;
			size_t prebufferSamples = 0; // samples to buffer before (re)starting
			size_t maxSamples = 0;       // cap to bound latency
			std::mutex mx;
			std::deque<float> buffer;
			bool playing = false;
		};

		ma_result streamRead(ma_data_source* ds, void* out, ma_uint64 frameCount, ma_uint64* framesRead)
		{
			auto* s = reinterpret_cast<StreamSource*>(ds);
			float* dst = static_cast<float*>(out);
			size_t want = static_cast<size_t>(frameCount) * s->channels;
			size_t got = 0;
			{
				std::lock_guard lk(s->mx);
				if (!s->playing && s->buffer.size() >= s->prebufferSamples)
					s->playing = true;
				if (s->playing)
				{
					got = std::min(want, s->buffer.size());
					std::copy_n(s->buffer.begin(), got, dst);
					s->buffer.erase(s->buffer.begin(), s->buffer.begin() + got);
					if (got < want)
						s->playing = false; // underrun: re-buffer
				}
			}
			std::fill(dst + got, dst + want, 0.0f);
			if (framesRead)
				*framesRead = frameCount;
			return MA_SUCCESS;
		}

		ma_result streamSeek(ma_data_source*, ma_uint64) { return MA_NOT_IMPLEMENTED; }

		ma_result streamGetFormat(ma_data_source* ds, ma_format* format, ma_uint32* channels,
			ma_uint32* sampleRate, ma_channel* channelMap, size_t channelMapCap)
		{
			auto* s = reinterpret_cast<StreamSource*>(ds);
			*format = ma_format_f32;
			*channels = s->channels;
			*sampleRate = s->sampleRate;
			if (channelMap)
				ma_channel_map_init_standard(ma_standard_channel_map_default, channelMap, channelMapCap, s->channels);
			return MA_SUCCESS;
		}

		ma_result streamGetCursor(ma_data_source*, ma_uint64* cursor)
		{
			*cursor = 0;
			return MA_NOT_IMPLEMENTED;
		}

		ma_result streamGetLength(ma_data_source*, ma_uint64* length)
		{
			*length = 0;
			return MA_NOT_IMPLEMENTED;
		}

		ma_data_source_vtable g_streamVtable = {
			streamRead, streamSeek, streamGetFormat, streamGetCursor, streamGetLength, nullptr, 0};

		std::mutex g_streamMx; // guards g_stream lifetime
		std::unique_ptr<StreamSource> g_stream;
		ma_sound g_streamSound;

		void destroyStreamLocked()
		{
			if (!g_stream)
				return;
			ma_sound_uninit(&g_streamSound);
			ma_data_source_uninit(&g_stream->base);
			g_stream.reset();
		}

		// ---- microphone ------------------------------------------------------
		std::mutex g_micMx;
		ma_device g_micDevice;
		bool g_micOk = false;
		MicSink g_micSink;
		uint32_t g_micChannels = 1;

		void micCallback(ma_device*, void*, const void* input, ma_uint32 frameCount)
		{
			if (g_micSink && input)
				g_micSink(static_cast<const int16_t*>(input), static_cast<size_t>(frameCount) * g_micChannels);
		}
	}

	bool init(float volume)
	{
		ma_engine_config cfg = ma_engine_config_init();
		ma_result r = ma_engine_init(&cfg, &g_engine);
		if (r != MA_SUCCESS)
		{
			LOG("audio: ma_engine_init failed (%d)", r);
			return false;
		}
		ma_engine_set_volume(&g_engine, volume);
		g_engineOk = true;
		char name[256] = "?";
		ma_device_get_name(ma_engine_get_device(&g_engine), ma_device_type_playback, name, sizeof(name), nullptr);
		LOG("audio: engine ready on '%s' (%u Hz, %u ch)", name, ma_engine_get_sample_rate(&g_engine), ma_engine_get_channels(&g_engine));
		return true;
	}

	void shutdown()
	{
		micStop();
		streamStop();
		stopClip();
		if (g_engineOk)
		{
			ma_engine_uninit(&g_engine);
			g_engineOk = false;
		}
	}

	bool playClipMemory(std::vector<uint8_t> data, float* lengthSec)
	{
		if (!g_engineOk || data.empty())
			return false;
		std::lock_guard lk(g_clipMx);
		destroyClipLocked();

		auto clip = std::make_unique<Clip>();
		clip->data = std::move(data);
		ma_result r = ma_decoder_init_memory(clip->data.data(), clip->data.size(), nullptr, &clip->decoder);
		if (r != MA_SUCCESS)
		{
			LOG("audio: cannot decode clip (%d) - expected wav/mp3/flac", r);
			return false;
		}
		clip->hasDecoder = true;
		r = ma_sound_init_from_data_source(&g_engine, &clip->decoder, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, &clip->sound);
		if (r != MA_SUCCESS)
		{
			LOG("audio: ma_sound_init_from_data_source failed (%d)", r);
			ma_decoder_uninit(&clip->decoder);
			return false;
		}
		if (lengthSec)
			*lengthSec = clipLength(&clip->sound);
		ma_sound_start(&clip->sound);
		g_clip = std::move(clip);
		return true;
	}

	bool playClipFile(const std::string& utf8Path, float* lengthSec)
	{
		if (!g_engineOk)
			return false;
		std::lock_guard lk(g_clipMx);
		destroyClipLocked();

		auto clip = std::make_unique<Clip>();
		ma_result r = ma_sound_init_from_file(&g_engine, utf8Path.c_str(),
			MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_DECODE, nullptr, nullptr, &clip->sound);
		if (r != MA_SUCCESS)
		{
			LOG("audio: cannot open '%s' (%d)", utf8Path.c_str(), r);
			return false;
		}
		if (lengthSec)
			*lengthSec = clipLength(&clip->sound);
		ma_sound_start(&clip->sound);
		g_clip = std::move(clip);
		return true;
	}

	bool isClipPlaying()
	{
		std::lock_guard lk(g_clipMx);
		return g_clip && !ma_sound_at_end(&g_clip->sound) && ma_sound_is_playing(&g_clip->sound);
	}

	void stopClip()
	{
		std::lock_guard lk(g_clipMx);
		destroyClipLocked();
	}

	bool streamStart(uint32_t sampleRate, uint32_t channels)
	{
		if (!g_engineOk || sampleRate < 8000 || sampleRate > 192000 || channels < 1 || channels > 2)
			return false;
		std::lock_guard lk(g_streamMx);
		destroyStreamLocked();

		auto s = std::make_unique<StreamSource>();
		s->sampleRate = sampleRate;
		s->channels = channels;
		s->prebufferSamples = sampleRate * channels / 10; // 100 ms
		s->maxSamples = sampleRate * channels * 2;        // 2 s

		ma_data_source_config dsCfg = ma_data_source_config_init();
		dsCfg.vtable = &g_streamVtable;
		ma_result r = ma_data_source_init(&dsCfg, &s->base);
		if (r != MA_SUCCESS)
			return false;
		r = ma_sound_init_from_data_source(&g_engine, &s->base, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, &g_streamSound);
		if (r != MA_SUCCESS)
		{
			LOG("audio: stream sound init failed (%d)", r);
			ma_data_source_uninit(&s->base);
			return false;
		}
		ma_sound_start(&g_streamSound);
		g_stream = std::move(s);
		LOG("audio: stream started (%u Hz, %u ch)", sampleRate, channels);
		return true;
	}

	void streamPush(const void* pcm16, size_t bytes)
	{
		std::lock_guard lk(g_streamMx);
		if (!g_stream)
			return;
		const auto* in = static_cast<const int16_t*>(pcm16);
		size_t n = bytes / sizeof(int16_t);
		std::lock_guard blk(g_stream->mx);
		auto& buf = g_stream->buffer;
		for (size_t i = 0; i < n; ++i)
			buf.push_back(in[i] / 32768.0f);
		if (buf.size() > g_stream->maxSamples)
		{
			// Too far behind real time: drop the oldest audio (keep channel alignment).
			size_t drop = buf.size() - g_stream->maxSamples;
			drop -= drop % g_stream->channels;
			buf.erase(buf.begin(), buf.begin() + drop);
		}
	}

	void streamStop()
	{
		std::lock_guard lk(g_streamMx);
		destroyStreamLocked();
	}

	bool micStart(uint32_t sampleRate, uint32_t channels, MicSink sink)
	{
		std::lock_guard lk(g_micMx);
		if (g_micOk)
			return true;
		ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
		cfg.capture.format = ma_format_s16;
		cfg.capture.channels = channels;
		cfg.sampleRate = sampleRate;
		cfg.periodSizeInMilliseconds = 20;
		cfg.dataCallback = micCallback;
		g_micSink = std::move(sink);
		g_micChannels = channels;
		ma_result r = ma_device_init(nullptr, &cfg, &g_micDevice);
		if (r != MA_SUCCESS)
		{
			LOG("audio: mic init failed (%d)", r);
			g_micSink = nullptr;
			return false;
		}
		if (ma_device_start(&g_micDevice) != MA_SUCCESS)
		{
			ma_device_uninit(&g_micDevice);
			g_micSink = nullptr;
			return false;
		}
		g_micOk = true;
		char name[256] = "?";
		ma_device_get_name(&g_micDevice, ma_device_type_capture, name, sizeof(name), nullptr);
		LOG("audio: mic '%s' started (%u Hz, %u ch)", name, sampleRate, channels);
		return true;
	}

	void micStop()
	{
		std::lock_guard lk(g_micMx);
		if (!g_micOk)
			return;
		ma_device_uninit(&g_micDevice); // stops and joins the capture thread
		g_micSink = nullptr;
		g_micOk = false;
	}
}
