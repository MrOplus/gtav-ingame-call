// HTTP (REST) + WebSocket API. Runs on IXWebSocket's own threads and only
// talks to the game through g_commands.
#pragma once

#include <string>

namespace api
{
	bool start();
	void stop();
	// Process exit: worker threads are already dead, so don't run destructors that
	// would join them or wait on them. The OS reclaims the memory and sockets.
	void abandon();

	// Send a JSON event to every authenticated WebSocket client.
	void broadcast(const std::string& json);

	// The connection whose binary frames feed the call stream / receive the mic.
	void setStreamOwner(const std::string& connectionId);
	void clearStreamOwner();
	void sendBinaryToOwner(const void* data, size_t bytes);
}
