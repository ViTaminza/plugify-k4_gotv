// k4_gotv — the Discord webhook notifier (see k4_discord.hpp).

#include "k4_discord.hpp"

#include "k4_discord_payload.hpp"
#include "k4_http.hpp"
#include "k4_worker.hpp"

#include <plg/plugin.hpp>

#include <chrono>
#include <ctime>
#include <deque>
#include <fstream>
#include <memory>
#include <string_view>
#include <utility>

namespace k4 {
namespace {

constexpr const char* kLogTag = "k4_gotv:";
constexpr std::size_t kMaximumAttachmentBytes = 100ull * 1024ull * 1024ull; // Discord boost level 3
constexpr auto kRequestTimeout = std::chrono::seconds(120);

void LogWarning(const std::string& message) {
	if (plg::Log) {
		plg::Log(std::string(kLogTag) + " " + message, plg::Severity::Warning, plg::source_location::current());
	}
}

bool ReadWholeFile(const std::string& path, std::string* out) {
	std::ifstream input(path, std::ios::binary);
	if (!input) {
		return false;
	}
	input.seekg(0, std::ios::end);
	const std::streamoff size = input.tellg();
	if (size < 0 || static_cast<uint64_t>(size) > kMaximumAttachmentBytes) {
		return false;
	}
	input.seekg(0, std::ios::beg);
	out->resize(static_cast<std::size_t>(size));
	if (size > 0) {
		input.read(out->data(), size);
		if (input.gcount() != size) {
			out->clear();
			return false;
		}
	}
	return true;
}

} // namespace

struct DiscordService::Impl {
	struct Message {
		std::string payload;
		std::string attachmentPath;
		std::string attachmentName;
		bool fallbackTried = false;
		std::function<void(bool, int)> done;
	};

	DiscordSettings settings;
	HttpService* http = nullptr;
	Worker* worker = nullptr;
	std::deque<Message> queue;
	bool busy = false;
	uint64_t generation = 0;
	unsigned boundaryCounter = 0;

	void StartNext();
	void SendJson(const Message& message);
	void SendWithAttachment(Message message, std::string boundary, std::string body);
	void Complete(const Message& message, const HttpResponse& response);
};

void DiscordService::Impl::Complete(const Message& message, const HttpResponse& response) {
	const bool ok = !response.transportFailed && response.status >= 200 && response.status <= 299;
	if (!ok && !message.attachmentPath.empty() && !message.fallbackTried
		&& (response.transportFailed || response.status == 413)) {
		// The multipart request itself was rejected (Steam's HTTP layer or Discord's 413).
		// Retry once with the embed only, so the notification and the S3 link still arrive.
		Message retry = message;
		retry.attachmentPath.clear();
		retry.attachmentName.clear();
		retry.fallbackTried = true;
		LogWarning("Discord rejected the multipart upload; retrying the embed without the file.");
		SendJson(retry);
		return;
	}
	if (!ok) {
		LogWarning("Discord webhook request failed ("
				   + (response.transportFailed ? std::string("network failure")
											   : "HTTP " + std::to_string(response.status))
				   + ").");
	}
	busy = false;
	if (message.done) {
		message.done(ok, response.transportFailed ? 0 : response.status);
	}
	StartNext();
}

void DiscordService::Impl::SendJson(const Message& message) {
	HttpRequest request;
	request.method = "POST";
	request.url = settings.webhookUrl;
	request.contentType = "application/json";
	request.body = message.payload;
	request.timeout = kRequestTimeout;
	request.onDone = [this, message](HttpResponse response) { Complete(message, response); };
	http->Enqueue(std::move(request));
}

void DiscordService::Impl::SendWithAttachment(Message message, std::string boundary, std::string body) {
	HttpRequest request;
	request.method = "POST";
	request.url = settings.webhookUrl;
	request.contentType = DiscordMultipartContentType(boundary);
	request.body = std::move(body);
	request.timeout = kRequestTimeout;
	request.onDone = [this, message](HttpResponse response) { Complete(message, response); };
	http->Enqueue(std::move(request));
}

void DiscordService::Impl::StartNext() {
	if (busy || queue.empty()) {
		return;
	}
	busy = true;
	Message message = std::move(queue.front());
	queue.pop_front();

	if (http == nullptr) {
		// Defensive: Configure() always supplies the service in this plugin.
		busy = false;
		if (message.done) {
			message.done(false, 0);
		}
		StartNext();
		return;
	}
	if (message.attachmentPath.empty() || worker == nullptr) {
		SendJson(message);
		return;
	}

	const uint64_t generationValue = generation;
	Impl* self = this;
	// The multipart body is assembled on the worker: with the attachment it is the size of the
	// demo, and copying a 100 MB zip into the request inside OnPluginUpdate would hitch the server.
	const std::string boundary = "----k4gotv" + std::to_string(++boundaryCounter) + std::to_string(std::time(nullptr));
	worker->Post([self, generationValue, boundary, message = std::move(message)]() mutable -> std::function<void()> {
		std::string data;
		const bool ok = ReadWholeFile(message.attachmentPath, &data);
		std::string body;
		if (ok) {
			body = BuildDiscordMultipart(boundary, message.payload, message.attachmentName, data);
		}
		return [self, generationValue, boundary, message = std::move(message), body = std::move(body), ok]() mutable {
			if (generationValue != self->generation) {
				return;
			}
			if (!ok) {
				LogWarning("Discord attachment '" + message.attachmentPath
						   + "' could not be read; sending the embed without the file.");
				self->SendJson(message);
				return;
			}
			self->SendWithAttachment(std::move(message), boundary, std::move(body));
		};
	});
}

DiscordService::DiscordService() : m_impl(std::make_unique<Impl>()) {}

DiscordService::~DiscordService() {
	Shutdown();
}

void DiscordService::Configure(DiscordSettings settings, HttpService* http, Worker* worker) {
	m_impl->settings = std::move(settings);
	m_impl->http = http;
	m_impl->worker = worker;
}

bool DiscordService::Enabled() const {
	return !m_impl->settings.webhookUrl.empty();
}

void DiscordService::Send(std::string payload, std::string attachmentPath, std::string attachmentName,
						  std::function<void(bool, int)> done) {
	Impl::Message message;
	message.payload = std::move(payload);
	message.attachmentPath = std::move(attachmentPath);
	message.attachmentName = std::move(attachmentName);
	message.done = std::move(done);
	m_impl->queue.push_back(std::move(message));
	m_impl->StartNext();
}

void DiscordService::Pump() {
	m_impl->StartNext();
}

void DiscordService::Shutdown() {
	++m_impl->generation;
	m_impl->queue.clear();
	m_impl->busy = false;
}

} // namespace k4
