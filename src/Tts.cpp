#include "Tts.h"

#include <windows.h>
#include <sapi.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "Log.h"

namespace tts
{
	namespace
	{
		std::wstring widen(const std::string& s)
		{
			int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
			std::wstring out(len > 0 ? len - 1 : 0, L'\0');
			MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
			return out;
		}

		template <typename T>
		struct ComPtr
		{
			T* p = nullptr;
			~ComPtr() { if (p) p->Release(); }
			T** operator&() { return &p; }
			T* operator->() { return p; }
		};

		void selectVoice(ISpVoice* voice, const std::string& name)
		{
			if (name.empty())
				return;
			ComPtr<ISpObjectTokenCategory> category;
			if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
					IID_ISpObjectTokenCategory, reinterpret_cast<void**>(&category))))
				return;
			if (FAILED(category->SetId(SPCAT_VOICES, FALSE)))
				return;
			std::wstring attrs = L"Name=" + widen(name);
			ComPtr<IEnumSpObjectTokens> tokens;
			if (FAILED(category->EnumTokens(attrs.c_str(), nullptr, &tokens)))
				return;
			ComPtr<ISpObjectToken> token;
			if (tokens->Next(1, &token, nullptr) == S_OK)
				voice->SetVoice(token.p);
			else
				LOG("tts: voice '%s' not found, using default", name.c_str());
		}

		Result synthesize(const std::string& text, const std::string& voiceName, int rate)
		{
			Result res;
			static std::atomic<int> counter{0};
			wchar_t tmpDir[MAX_PATH];
			GetTempPathW(MAX_PATH, tmpDir);
			std::wstring path = std::wstring(tmpDir) + L"phonelink_tts_" +
				std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(counter++) + L".wav";

			HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			{
				ComPtr<ISpVoice> voice;
				ComPtr<ISpStream> stream;
				HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, reinterpret_cast<void**>(&voice));
				if (SUCCEEDED(hr))
					hr = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_ISpStream, reinterpret_cast<void**>(&stream));
				if (SUCCEEDED(hr))
				{
					WAVEFORMATEX wfx{};
					wfx.wFormatTag = WAVE_FORMAT_PCM;
					wfx.nChannels = 1;
					wfx.nSamplesPerSec = 22050;
					wfx.wBitsPerSample = 16;
					wfx.nBlockAlign = 2;
					wfx.nAvgBytesPerSec = 44100;
					hr = stream->BindToFile(path.c_str(), SPFM_CREATE_ALWAYS, &SPDFID_WaveFormatEx, &wfx, 0);
				}
				if (SUCCEEDED(hr))
				{
					selectVoice(voice.p, voiceName);
					voice->SetRate(rate);
					hr = voice->SetOutput(stream.p, TRUE);
				}
				if (SUCCEEDED(hr))
					hr = voice->Speak(widen(text).c_str(), SPF_DEFAULT | SPF_IS_NOT_XML, nullptr);
				if (stream.p)
					stream->Close();
				if (FAILED(hr))
				{
					char buf[64];
					snprintf(buf, sizeof(buf), "SAPI error 0x%08lX", static_cast<unsigned long>(hr));
					res.error = buf;
				}
			}
			if (SUCCEEDED(hrInit))
				CoUninitialize();

			if (res.error.empty())
			{
				std::ifstream f(path, std::ios::binary);
				res.wav.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
				res.ok = res.wav.size() > 44;
				if (!res.ok)
					res.error = "SAPI produced no audio";
			}
			DeleteFileW(path.c_str());
			return res;
		}
	}

	std::future<Result> synthesizeAsync(std::string utf8Text, std::string voiceName, int rate)
	{
		return std::async(std::launch::async, [text = std::move(utf8Text), voice = std::move(voiceName), rate] {
			uint64_t t0 = GetTickCount64();
			Result r = synthesize(text, voice, rate);
			LOG_INFO("tts: %zu chars -> %s in %llums", text.size(), r.ok ? "ok" : r.error.c_str(), GetTickCount64() - t0);
			return r;
		});
	}
}
