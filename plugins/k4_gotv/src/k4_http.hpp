// k4_gotv — asynchronous HTTP with two transports.
//
// 1. libcurl (k4_curl.cpp), dlopen'ed at runtime, HTTP/1.1, no Expect: 100-continue. This is the
//    default (Http.transport "auto"): on the live server ISteamHTTP delivered every S3 PUT but
//    then reported a client-side "no response" failure and dropped the response, so an upload
//    must not depend on it.
// 2. Steam: the server's own libsteam_api.so (dlopen + the documented SteamAPI_ISteamHTTP_*
//    accessors + a CCallbackBase-layout call result), copied from plugins/cs2ac/src/webhook.cpp.
//    Used only when libcurl cannot be loaded, or when Http.transport is explicitly "steam".
//
// One request is in flight at a time. Enqueue() never blocks; Pump() (OnPluginUpdate, game
// thread) starts the next request and delivers completed responses to their callbacks on the
// game thread. Steam call results are pumped by the engine; curl requests run on a dedicated
// thread owned by this service and are handed back through a mutex-protected completion queue.
//
// The service is shared by the S3 client and the Discord notifier. It reads response bodies and
// the response headers the caller asked for (S3 needs ETag), and it never logs request headers
// (they carry the Authorization header).

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace k4 {

enum class HttpTransport {
	Auto,  // libcurl when it loads on this host; ISteamHTTP only as the fallback
	Steam, // ISteamHTTP only
	Curl,  // libcurl only
};

struct HttpHeader {
	std::string name;
	std::string value;
};

struct HttpResponse {
	int status = 0;
	bool transportFailed = true; // no HTTP response: DNS/TCP/TLS/send/receive/timeout/abort
	std::string body;
	// Response headers that were requested through HttpRequest::captureHeaders, in that order
	// (missing ones are omitted).
	std::vector<std::pair<std::string, std::string>> headers;

	// Diagnostics (never contain credentials).
	std::string transport;			// "steam" or "curl"
	bool ioFailure = false;			// Steam: bIOFailure in the call result
	bool requestSuccessful = false; // Steam: m_bRequestSuccessful
	// Human-readable transport failure, e.g. "curl error 6: Couldn't resolve host name" or
	// "no response from the server (io failure 1, successful 0, status 0)".
	std::string transportError;
};

struct HttpRequest {
	std::string method; // GET / HEAD / POST / PUT / DELETE
	std::string url;
	std::vector<HttpHeader> headers;
	// Content type of `body` (SetHTTPRequestRawPostBody's second argument). Required when the
	// body is non-empty; ignored for an empty body.
	std::string contentType;
	std::string body;
	std::vector<std::string> captureHeaders; // e.g. {"ETag"}
	std::chrono::milliseconds timeout {120000};
	std::function<void(HttpResponse)> onDone; // called from Pump() on the game thread

	// Internal bookkeeping is deliberately minimal: a request is attempted exactly once (the
	// Steam-transport upload must never be replayed - the live server proved the PUT already
	// arrived while Steam lost the response, and a replay duplicates the object).
};

class HttpService {
public:
	// Implementation detail (defined in k4_http.cpp); public so the Steam call-result trampoline
	// and the curl worker can name it, like k4_missions' WebhookService::Impl.
	struct Impl;

	HttpService();
	~HttpService();
	HttpService(const HttpService&) = delete;
	HttpService& operator=(const HttpService&) = delete;

	// Game thread. Applies to subsequent requests; safe to call while one is in flight.
	void Configure(HttpTransport transport, std::string caFile);

	void Enqueue(HttpRequest request);
	void Pump();
	// Cancels the in-flight request and drops the queue (plugin unload / reload). The pending
	// callbacks are NOT invoked.
	void Shutdown();
	bool Busy() const;
	std::size_t Pending() const;

private:
	std::unique_ptr<Impl> m_impl;
};

} // namespace k4
