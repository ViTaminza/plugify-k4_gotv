// k4_gotv — optional HTTP transport through libcurl, loaded with dlopen at runtime.
//
// Why: ISteamHTTP (the transport the plugin started with) fails every real upload on the live
// server with a client-side network failure (no HTTP response at all), while the same S3 endpoint
// accepts the equivalent rclone/curl request. s2sdk exposes no HTTP API and the plugin must not
// link a distribution libcurl (the game runs in an old Steam Runtime and the plugin is built with
// a static libstdc++), so libcurl is dlopen'ed by soname and driven through its stable C ABI.
// The game's Steam Runtime 3 ships libcurl-gnutls.so.4 (steamclient dependency); the gnutls build
// is tried first so no OpenSSL symbols from the game are involved.
//
// CurlPerform() is synchronous and thread-agnostic: the plugin runs it on its own HTTP thread,
// the host-side probe tool runs it directly. It never logs and never touches engine state.
//
// The curl option numbers below are the stable values from curl's easy.h (they are part of the
// ABI, not an internal detail); the bundled sandbox image has the runtime library but not the
// development headers, and linking a distro libcurl would add an unwanted DT_NEEDED entry.

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace k4 {

struct CurlHeader {
	std::string name;
	std::string value;
};

struct CurlRequest {
	std::string method; // GET / HEAD / POST / PUT / DELETE
	std::string url;	// https:// only
	std::vector<CurlHeader> headers;
	std::string contentType; // Content-Type for a non-empty body
	std::string body;
	std::vector<std::string> captureHeaders; // response headers to return
	std::chrono::milliseconds timeout {120000};
};

struct CurlResponse {
	bool transportFailed = true; // curl_easy_perform() failed: DNS/TCP/TLS/send/recv/abort/timeout
	int status = 0;				 // HTTP status when a response was received
	std::string body;			 // capped at 4 MiB
	std::vector<std::pair<std::string, std::string>> headers; // requested headers, in request order
	// "curl error 6: Couldn't resolve host name" or "" — never contains credentials (libcurl
	// error strings describe the failure only).
	std::string error;
};

// True when libcurl was loaded (fills *version with "7.64.0" style text when non-null).
bool CurlAvailable(std::string* version = nullptr);

// Performs one request. `caFile` empty = libcurl's compiled-in CA bundle. `abort` (may be null)
// is polled by curl's progress callback; setting it makes the call return promptly.
// Must not be called concurrently for the same abort flag; libcurl handles are per call.
CurlResponse CurlPerform(const CurlRequest& request, const std::string& caFile, const std::atomic<bool>* abort);

} // namespace k4
