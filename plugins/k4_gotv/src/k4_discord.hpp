// k4_gotv — the Discord webhook notifier.
//
// The original POSTed MultipartFormDataContent with payload_json and, when the zipped demo fit
// the server-boost file limit, the .zip itself. This service does the same over HttpService:
// JSON only when there is no attachment, multipart/form-data otherwise. The attachment is read on
// the Worker thread; a failed read falls back to the JSON-only post (the embed still carries the
// S3 link). One message is queued at a time, like the original's sequential processing.

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "k4_core.hpp"

namespace k4 {

class HttpService;
class Worker;

class DiscordService {
public:
	DiscordService();
	~DiscordService();
	DiscordService(const DiscordService&) = delete;
	DiscordService& operator=(const DiscordService&) = delete;

	void Configure(DiscordSettings settings, HttpService* http, Worker* worker);
	bool Enabled() const; // WebhookURL non-empty

	// `attachmentPath` may be empty. `done(ok, status)` runs on the game thread.
	void Send(std::string payload, std::string attachmentPath, std::string attachmentName,
			  std::function<void(bool, int)> done);

	void Pump();
	void Shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace k4
