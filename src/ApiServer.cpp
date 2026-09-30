#include "ApiServer.h"

#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <windows.h>

#include "json.hpp"

#include "Audio.h"
#include "Base64.h"
#include "Commands.h"
#include "Config.h"
#include "Log.h"
#include "Script.h"

using json = nlohmann::json;

namespace api
{
	namespace
	{
		std::unique_ptr<ix::HttpServer> g_http;
		std::unique_ptr<ix::WebSocketServer> g_ws;

		// Guards g_authed / g_streamOwner. Never call into IXWebSocket while holding it:
		// a failed send invokes the Close callback synchronously, which takes this lock.
		std::mutex g_connMx;
		std::map<std::string, ix::WebSocket*> g_authed; // connection id -> socket (identity only)
		std::string g_streamOwner;

		// Outgoing messages are sent from one sender thread so the game thread and the
		// audio thread never block on (or fail inside) network sends.
		struct Outgoing
		{
			std::string data;
			bool binary = false;
			bool ownerOnly = false;
		};
		std::mutex g_outMx;
		std::condition_variable g_outCv;
		std::deque<Outgoing> g_out;
		std::thread g_sender;
		bool g_senderStop = false;
		constexpr size_t kMaxQueuedBinary = 200; // ~4 s of 20 ms mic frames

		void enqueue(Outgoing item)
		{
			{
				std::lock_guard lk(g_outMx);
				if (item.binary)
				{
					size_t binaries = 0;
					for (auto& o : g_out)
						binaries += o.binary;
					if (binaries >= kMaxQueuedBinary)
					{
						for (auto it = g_out.begin(); it != g_out.end(); ++it)
							if (it->binary)
							{
								g_out.erase(it); // drop the oldest audio rather than grow without bound
								break;
							}
					}
				}
				g_out.push_back(std::move(item));
			}
			g_outCv.notify_one();
		}

		void deliver(const Outgoing& item)
		{
			if (!g_ws)
				return;
			// getClients() hands out shared_ptrs, keeping each socket alive while we send.
			std::vector<std::shared_ptr<ix::WebSocket>> targets;
			auto clients = g_ws->getClients();
			{
				std::lock_guard lk(g_connMx);
				ix::WebSocket* owner = nullptr;
				if (item.ownerOnly)
				{
					auto it = g_authed.find(g_streamOwner);
					owner = it != g_authed.end() ? it->second : nullptr;
				}
				for (auto& c : clients)
				{
					bool authed = false;
					for (auto& [id, ws] : g_authed)
						authed |= (ws == c.get());
					if (item.ownerOnly ? (owner == c.get()) : authed)
						targets.push_back(c);
				}
			}
			for (auto& t : targets)
			{
				if (item.binary)
					t->sendBinary(item.data);
				else
					t->sendText(item.data);
			}
		}

		void senderLoop()
		{
			while (true)
			{
				std::deque<Outgoing> batch;
				{
					std::unique_lock lk(g_outMx);
					g_outCv.wait(lk, [] { return g_senderStop || !g_out.empty(); });
					if (g_senderStop)
						return;
					batch.swap(g_out);
				}
				for (auto& item : batch)
				{
					try
					{
						deliver(item);
					}
					catch (const std::exception& e)
					{
						LOG_ERROR("ws: send failed: %s", e.what());
					}
				}
			}
		}

		std::atomic<uint64_t> g_nextId{1};

		std::string newCallId()
		{
			return "call-" + std::to_string(g_nextId++);
		}

		bool tokenOk(const std::string& presented)
		{
			return g_config.token.empty() || presented == g_config.token;
		}

		std::string bearer(const ix::WebSocketHttpHeaders& headers)
		{
			if (auto it = headers.find("Authorization"); it != headers.end())
			{
				const std::string& v = it->second;
				return v.rfind("Bearer ", 0) == 0 ? v.substr(7) : v;
			}
			if (auto it = headers.find("X-Api-Token"); it != headers.end())
				return it->second;
			return {};
		}

		std::string queryParam(const std::string& uri, const std::string& key)
		{
			auto q = uri.find('?');
			if (q == std::string::npos)
				return {};
			std::string query = uri.substr(q + 1);
			size_t pos = 0;
			while (pos <= query.size())
			{
				size_t amp = query.find('&', pos);
				std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
				auto eq = pair.find('=');
				if (eq != std::string::npos && pair.substr(0, eq) == key)
					return pair.substr(eq + 1);
				if (amp == std::string::npos)
					break;
				pos = amp + 1;
			}
			return {};
		}

		std::string str(const json& j, const char* key, const std::string& def = {})
		{
			auto it = j.find(key);
			return (it != j.end() && it->is_string()) ? it->get<std::string>() : def;
		}

		template <typename T>
		T num(const json& j, const char* key, T def)
		{
			auto it = j.find(key);
			return (it != j.end() && it->is_number()) ? it->get<T>() : def;
		}

		bool flag(const json& j, const char* key, bool def = false)
		{
			auto it = j.find(key);
			return (it != j.end() && it->is_boolean()) ? it->get<bool>() : def;
		}

		// Parses an API request into a command. `connectionId` is set for
		// WebSocket requests (required for streaming calls).
		// Returns an error message, or empty on success; `reply` gets response fields.
		std::string parseCommand(const std::string& type, const json& j, const std::string& connectionId, json& reply)
		{
			if (type == "sms")
			{
				SmsCommand c;
				c.from = str(j, "from", "Unknown");
				c.subject = str(j, "subject");
				c.text = str(j, "text");
				c.icon = str(j, "icon", g_config.defaultIcon);
				c.flash = flag(j, "flash");
				if (c.text.empty())
					return "'text' is required";
				g_commands.push(std::move(c));
				return {};
			}

			if (type == "call")
			{
				CallCommand c;
				c.id = str(j, "id", newCallId());
				c.from = str(j, "from", "Unknown");
				c.icon = str(j, "icon", g_config.defaultIcon);
				c.text = str(j, "text");
				c.ringTimeoutSec = num<int>(j, "ring_timeout", g_config.ringTimeoutSec);

				if (flag(j, "stream"))
				{
					if (connectionId.empty())
						return "streaming calls are only available over the WebSocket API";
					c.audio = CallAudio::Stream;
					c.ownerConnection = connectionId;
					c.sampleRate = num<uint32_t>(j, "sample_rate", 24000);
					c.channels = num<uint32_t>(j, "channels", 1);
					c.mic = flag(j, "mic");
					c.micSampleRate = num<uint32_t>(j, "mic_sample_rate", g_config.micSampleRate);
					c.outgoing = flag(j, "outgoing");
					if (c.channels < 1 || c.channels > 2)
						return "'channels' must be 1 or 2";
				}
				else if (auto b64 = str(j, "audio_base64"); !b64.empty())
				{
					auto bytes = base64Decode(b64);
					if (!bytes || bytes->empty())
						return "'audio_base64' is not valid base64";
					c.audio = CallAudio::Clip;
					c.clip = std::move(*bytes);
				}
				else if (auto path = str(j, "audio_path"); !path.empty())
				{
					c.audio = CallAudio::File;
					c.filePath = path;
				}
				else if (!c.text.empty())
				{
					c.audio = CallAudio::Tts;
				}
				else
				{
					return "provide one of: 'text' (TTS), 'audio_base64', 'audio_path', or 'stream': true";
				}

				reply["id"] = c.id;
				g_commands.push(std::move(c));
				return {};
			}

			if (type == "subtitle")
			{
				SubtitleCommand c;
				c.id = str(j, "id");
				c.text = str(j, "text");
				c.durationMs = num<int>(j, "duration_ms", 3000);
				if (c.text.empty())
					return "'text' is required";
				g_commands.push(std::move(c));
				return {};
			}

			if (type == "hangup")
			{
				g_commands.push(HangupCommand{str(j, "id"), str(j, "reason")});
				return {};
			}

			if (type == "connect")
			{
				std::string id = str(j, "id");
				if (id.empty())
					return "'id' is required";
				g_commands.push(ConnectCommand{id});
				return {};
			}

			if (type == "contacts")
			{
				ContactsCommand c;
				auto it = j.find("contacts");
				if (it != j.end() && it->is_array())
				{
					for (auto& e : *it)
					{
						if (e.is_object() && !str(e, "id").empty())
							c.contacts.push_back({str(e, "id"), str(e, "name", "Friend")});
					}
				}
				g_commands.push(std::move(c));
				return {};
			}

			return "unknown type '" + type + "'";
		}

		json statusJson()
		{
			auto s = script::status();
			return {
				{"version", PHONELINK_VERSION},
				{"in_game", s.inGame},
				{"call_state", s.callState},
				{"call_id", s.callId},
			};
		}

		// ---- HTTP ------------------------------------------------------------
		ix::HttpResponsePtr respond(int code, const json& body)
		{
			ix::WebSocketHttpHeaders headers;
			headers["Content-Type"] = "application/json";
			headers["Access-Control-Allow-Origin"] = "*";
			headers["Access-Control-Allow-Headers"] = "Content-Type, Authorization, X-Api-Token";
			headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS";
			const char* desc = code < 300 ? "OK" : (code == 401 ? "Unauthorized" : (code == 404 ? "Not Found" : "Bad Request"));
			return std::make_shared<ix::HttpResponse>(code, desc, ix::HttpErrorCode::Ok, headers, body.dump());
		}

		ix::HttpResponsePtr routeHttp(const ix::HttpRequestPtr& req);

		ix::HttpResponsePtr handleHttp(ix::HttpRequestPtr req, std::shared_ptr<ix::ConnectionState> state)
		{
			uint64_t t0 = GetTickCount64();
			ix::HttpResponsePtr res;
			try
			{
				res = routeHttp(req);
			}
			catch (const std::exception& e)
			{
				LOG_ERROR("http: handler error: %s", e.what());
				res = respond(500, {{"ok", false}, {"error", "internal error"}});
			}
			std::string path = req->uri.substr(0, req->uri.find('?')); // never log the token query
			if (res->statusCode >= 400)
				LOG_WARN("http %s %s from %s -> %d %s", req->method.c_str(), path.c_str(), state->getRemoteIp().c_str(),
					res->statusCode, res->body.c_str());
			else
				LOG_INFO("http %s %s from %s -> %d (%zu bytes in, %llums)", req->method.c_str(), path.c_str(),
					state->getRemoteIp().c_str(), res->statusCode, req->body.size(), GetTickCount64() - t0);
			return res;
		}

		ix::HttpResponsePtr routeHttp(const ix::HttpRequestPtr& req)
		{
			std::string path = req->uri.substr(0, req->uri.find('?'));
			if (req->method == "OPTIONS")
				return respond(204, json::object());
			if (!tokenOk(bearer(req->headers)) && !tokenOk(queryParam(req->uri, "token")))
				return respond(401, {{"ok", false}, {"error", "unauthorized"}});

			if (req->method == "GET" && (path == "/status" || path == "/"))
				return respond(200, statusJson());

			if (req->method != "POST")
				return respond(404, {{"ok", false}, {"error", "not found"}});

			std::string type = path.size() > 1 ? path.substr(1) : "";
			json body = json::parse(req->body.empty() ? "{}" : req->body, nullptr, false);
			if (body.is_discarded() || !body.is_object())
				return respond(400, {{"ok", false}, {"error", "body must be a JSON object"}});

			json reply = {{"ok", true}};
			std::string err = parseCommand(type, body, {}, reply);
			if (!err.empty())
				return respond(err.rfind("unknown type", 0) == 0 ? 404 : 400, {{"ok", false}, {"error", err}});
			return respond(202, reply);
		}

		// ---- WebSocket -------------------------------------------------------
		void handleWsUnsafe(std::shared_ptr<ix::ConnectionState> state, ix::WebSocket& ws, const ix::WebSocketMessagePtr& msg);

		void handleWs(std::shared_ptr<ix::ConnectionState> state, ix::WebSocket& ws, const ix::WebSocketMessagePtr& msg)
		{
			try
			{
				handleWsUnsafe(std::move(state), ws, msg);
			}
			catch (const std::exception& e)
			{
				LOG_ERROR("ws: handler error: %s", e.what());
			}
		}

		void handleWsUnsafe(std::shared_ptr<ix::ConnectionState> state, ix::WebSocket& ws, const ix::WebSocketMessagePtr& msg)
		{
			const std::string id = state->getId();
			switch (msg->type)
			{
			case ix::WebSocketMessageType::Open:
			{
				if (!tokenOk(bearer(msg->openInfo.headers)) && !tokenOk(queryParam(msg->openInfo.uri, "token")))
				{
					LOG_WARN("ws: rejected unauthenticated client %s (%s)", id.c_str(), state->getRemoteIp().c_str());
					ws.close(4001, "unauthorized");
					return;
				}
				{
					std::lock_guard lk(g_connMx);
					g_authed[id] = &ws;
				}
				LOG_INFO("ws: client %s connected from %s", id.c_str(), state->getRemoteIp().c_str());
				json hello = statusJson();
				hello["event"] = "hello";
				ws.sendText(hello.dump());
				break;
			}
			case ix::WebSocketMessageType::Close:
			{
				bool wasOwner;
				{
					std::lock_guard lk(g_connMx);
					g_authed.erase(id);
					wasOwner = (g_streamOwner == id);
				}
				LOG_INFO("ws: client %s disconnected (code %d %s)%s", id.c_str(), msg->closeInfo.code,
					msg->closeInfo.reason.c_str(), wasOwner ? " - was streaming, ending call" : "");
				if (wasOwner)
					g_commands.push(HangupCommand{}); // the far end of a live call went away
				break;
			}
			case ix::WebSocketMessageType::Message:
			{
				{
					std::lock_guard lk(g_connMx);
					if (!g_authed.count(id))
						return;
					if (msg->binary)
					{
						if (g_streamOwner == id)
							audio::streamPush(msg->str.data(), msg->str.size());
						else
							LOG_DEBUG("ws: dropped %zu audio bytes from %s (not the active stream owner)", msg->str.size(), id.c_str());
						return;
					}
				}

				json j = json::parse(msg->str, nullptr, false);
				json reply = {{"event", "ack"}, {"ok", true}};
				if (j.is_discarded() || !j.is_object())
				{
					reply["ok"] = false;
					reply["error"] = "message must be a JSON object";
					LOG_WARN("ws: client %s sent invalid JSON (%zu bytes)", id.c_str(), msg->str.size());
				}
				else
				{
					if (j.contains("req_id"))
						reply["req_id"] = j["req_id"];
					std::string type = str(j, "type");
					if (type == "status")
					{
						json s = statusJson();
						s["event"] = "status";
						if (j.contains("req_id"))
							s["req_id"] = j["req_id"];
						ws.sendText(s.dump());
						return;
					}
					LOG_INFO("ws: client %s sent '%s'", id.c_str(), type.c_str());
					std::string err = parseCommand(type, j, id, reply);
					if (!err.empty())
					{
						LOG_WARN("ws: client %s request '%s' rejected: %s", id.c_str(), type.c_str(), err.c_str());
						reply["ok"] = false;
						reply["error"] = err;
					}
				}
				ws.sendText(reply.dump());
				break;
			}
			case ix::WebSocketMessageType::Error:
				LOG_ERROR("ws: error on %s: %s", id.c_str(), msg->errorInfo.reason.c_str());
				break;
			default:
				break;
			}
		}
	}

	bool start()
	{
		ix::initNetSystem();

		g_http = std::make_unique<ix::HttpServer>(g_config.httpPort, g_config.host);
		g_http->setOnConnectionCallback(handleHttp);
		auto res = g_http->listen();
		if (!res.first)
		{
			LOG("api: HTTP listen on %s:%d failed: %s", g_config.host.c_str(), g_config.httpPort, res.second.c_str());
			g_http.reset();
		}
		else
		{
			g_http->start();
			LOG("api: HTTP listening on http://%s:%d", g_config.host.c_str(), g_config.httpPort);
		}

		g_ws = std::make_unique<ix::WebSocketServer>(g_config.wsPort, g_config.host);
		g_ws->disablePerMessageDeflate();
		g_ws->setOnClientMessageCallback(handleWs);
		res = g_ws->listen();
		if (!res.first)
		{
			LOG("api: WebSocket listen on %s:%d failed: %s", g_config.host.c_str(), g_config.wsPort, res.second.c_str());
			g_ws.reset();
		}
		else
		{
			g_ws->start();
			LOG("api: WebSocket listening on ws://%s:%d", g_config.host.c_str(), g_config.wsPort);
		}
		g_senderStop = false;
		g_sender = std::thread(senderLoop);
		return g_http || g_ws;
	}

	void stop()
	{
		{
			std::lock_guard lk(g_outMx);
			g_senderStop = true;
		}
		g_outCv.notify_all();
		if (g_sender.joinable())
			g_sender.join();
		if (g_ws)
			g_ws->stop();
		if (g_http)
			g_http->stop();
		g_ws.reset();
		g_http.reset();
		ix::uninitNetSystem();
	}

	void abandon()
	{
		if (g_sender.joinable())
			g_sender.detach();
		(void)g_ws.release();
		(void)g_http.release();
	}

	void broadcast(const std::string& text)
	{
		enqueue({text, false, false});
	}

	void setStreamOwner(const std::string& connectionId)
	{
		std::lock_guard lk(g_connMx);
		g_streamOwner = connectionId;
	}

	void clearStreamOwner()
	{
		std::lock_guard lk(g_connMx);
		g_streamOwner.clear();
	}

	void sendBinaryToOwner(const void* data, size_t bytes)
	{
		enqueue({std::string(static_cast<const char*>(data), bytes), true, true});
	}
}
