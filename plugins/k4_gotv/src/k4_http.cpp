// k4_gotv — asynchronous HTTP over ISteamHTTP with an optional libcurl fallback (see k4_http.hpp).
//
// The Steam half is copied from plugins/cs2ac/src/webhook.cpp (dlopen + the documented
// SteamAPI_ISteamHTTP_* C accessors + a CCallbackBase-layout call result). The curl half runs on
// its own thread (libcurl is blocking) and hands completed requests back through a mutex-protected
// queue; both transports deliver their callbacks from Pump() on the game thread.

#include "k4_http.hpp"

#include "k4_text.hpp"

#include "k4_curl.hpp"

#include <plg/plugin.hpp>
#include <s2sdk/s2sdk.hpp>

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace k4 {
namespace {

constexpr const char* kLogTag = "k4_gotv:";
constexpr uint32_t kMaximumResponseBody = 4u * 1024u * 1024u;
constexpr uint32_t kMaximumHeaderValue = 64u * 1024u;

void LogInfo(const std::string& message) {
	if (plg::Log) {
		plg::Log(std::string(kLogTag) + " " + message, plg::Severity::Info, plg::source_location::current());
	}
}

void LogWarning(const std::string& message) {
	if (plg::Log) {
		plg::Log(std::string(kLogTag) + " " + message, plg::Severity::Warning, plg::source_location::current());
	}
}

void LogError(const std::string& message) {
	if (plg::Log) {
		plg::Log(std::string(kLogTag) + " " + message, plg::Severity::Error, plg::source_location::current());
	}
}

// HTTPRequestCompleted_t (isteamhttp.h) — the same layout cs2ac verified against the SDK.
struct HTTPRequestCompleted_t {
	enum { k_iCallback = 2102 }; // k_iSteamHTTPCallbacks (2101) + 1
	uint32_t m_hRequest;
	uint64_t m_ulContextValue;
	bool m_bRequestSuccessful;
	int32_t m_eStatusCode;
	uint32_t m_unBodySize;
};
static_assert(sizeof(HTTPRequestCompleted_t) == 32, "HTTPRequestCompleted_t layout");
static_assert(offsetof(HTTPRequestCompleted_t, m_eStatusCode) == 20, "HTTPRequestCompleted_t layout");

// Valve's CCallbackBase layout: vtable + the two data members right after it.
class CallbackBase {
public:
	virtual void Run(void* pvParam) = 0;
	virtual void Run(void* pvParam, bool bIOFailure, uint64_t hSteamAPICall) = 0;
	virtual int GetCallbackSizeBytes() = 0;

	enum {
		k_ECallbackFlagsRegistered = 0x01,
		k_ECallbackFlagsGameServer = 0x02,
	};
	uint8_t m_nCallbackFlags;
	int32_t m_iCallback;

protected:
	CallbackBase() : m_nCallbackFlags(0), m_iCallback(0) {}
	~CallbackBase() = default;
};

struct SteamHttpApi {
	void* module {};
	void* (*getHttp)() {};
	uint32_t (*CreateHTTPRequest)(void*, int32_t, const char*) {};
	bool (*SetHTTPRequestHeaderValue)(void*, uint32_t, const char*, const char*) {};
	bool (*SetHTTPRequestRawPostBody)(void*, uint32_t, const char*, uint8_t*, uint32_t) {};
	bool (*SendHTTPRequest)(void*, uint32_t, uint64_t*) {};
	// Optional (SDK-version dependent): inactivity/absolute timeouts. The documented default
	// inactivity timeout is 60 s, which is short for a multi-MB upload on a slow link.
	bool (*SetHTTPRequestNetworkActivityTimeout)(void*, uint32_t, uint32_t) {};
	bool (*SetHTTPRequestAbsoluteTimeoutMS)(void*, uint32_t, uint32_t) {};
	bool (*GetHTTPResponseBodySize)(void*, uint32_t, uint32_t*) {};
	bool (*GetHTTPResponseBodyData)(void*, uint32_t, uint8_t*, uint32_t) {};
	bool (*GetHTTPResponseHeaderSize)(void*, uint32_t, const char*, uint32_t*) {};
	bool (*GetHTTPResponseHeaderValue)(void*, uint32_t, const char*, uint8_t*, uint32_t) {};
	bool (*ReleaseHTTPRequest)(void*, uint32_t) {};
	void (*RegisterCallResult)(CallbackBase*, uint64_t) {};
	void (*UnregisterCallResult)(CallbackBase*, uint64_t) {};
	void* http {};
};

SteamHttpApi g_steam;
bool g_symbolFailure {};
bool g_loadWarningLogged {};

template<typename Fn>
Fn ResolveSymbol(void* module, const char* name) {
	return reinterpret_cast<Fn>(dlsym(module, name));
}

bool ResolveSteamHttp() {
	if (g_symbolFailure) {
		return false;
	}
	if (g_steam.getHttp == nullptr) {
		std::vector<std::string> candidates = {"libsteam_api.so"};
		if (__s2sdk_GetGameDirectory) {
			const std::string gameDir(s2sdk::GetGameDirectory());
			if (!gameDir.empty()) {
				candidates.push_back(gameDir + "/bin/linuxsteamrt64/libsteam_api.so");
				candidates.push_back(gameDir + "/../bin/linuxsteamrt64/libsteam_api.so");
			}
		}
		candidates.push_back("bin/linuxsteamrt64/libsteam_api.so");
		for (const std::string& candidate : candidates) {
			if (void* handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_NOLOAD)) {
				g_steam.module = handle;
				break;
			}
		}
		if (g_steam.module == nullptr) {
			for (const std::string& candidate : candidates) {
				if (void* handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL)) {
					g_steam.module = handle;
					break;
				}
			}
		}
		if (g_steam.module == nullptr) {
			if (!g_loadWarningLogged) {
				g_loadWarningLogged = true;
				LogWarning("libsteam_api.so could not be loaded; the Steam HTTP transport stays disabled.");
			}
			return false;
		}
		for (const char* name : {"SteamAPI_SteamGameServerHTTP_v004", "SteamAPI_SteamGameServerHTTP_v003",
								 "SteamAPI_SteamGameServerHTTP_v002"}) {
			g_steam.getHttp = ResolveSymbol<decltype(g_steam.getHttp)>(g_steam.module, name);
			if (g_steam.getHttp) {
				break;
			}
		}
		g_steam.CreateHTTPRequest = ResolveSymbol<decltype(g_steam.CreateHTTPRequest)>(g_steam.module, "SteamAPI_ISteamHTTP_CreateHTTPRequest");
		g_steam.SetHTTPRequestHeaderValue =
			ResolveSymbol<decltype(g_steam.SetHTTPRequestHeaderValue)>(g_steam.module, "SteamAPI_ISteamHTTP_SetHTTPRequestHeaderValue");
		g_steam.SetHTTPRequestRawPostBody =
			ResolveSymbol<decltype(g_steam.SetHTTPRequestRawPostBody)>(g_steam.module, "SteamAPI_ISteamHTTP_SetHTTPRequestRawPostBody");
		g_steam.SendHTTPRequest = ResolveSymbol<decltype(g_steam.SendHTTPRequest)>(g_steam.module, "SteamAPI_ISteamHTTP_SendHTTPRequest");
		g_steam.SetHTTPRequestNetworkActivityTimeout = ResolveSymbol<decltype(g_steam.SetHTTPRequestNetworkActivityTimeout)>(
			g_steam.module, "SteamAPI_ISteamHTTP_SetHTTPRequestNetworkActivityTimeout");
		g_steam.SetHTTPRequestAbsoluteTimeoutMS = ResolveSymbol<decltype(g_steam.SetHTTPRequestAbsoluteTimeoutMS)>(
			g_steam.module, "SteamAPI_ISteamHTTP_SetHTTPRequestAbsoluteTimeoutMS");
		g_steam.GetHTTPResponseBodySize =
			ResolveSymbol<decltype(g_steam.GetHTTPResponseBodySize)>(g_steam.module, "SteamAPI_ISteamHTTP_GetHTTPResponseBodySize");
		g_steam.GetHTTPResponseBodyData =
			ResolveSymbol<decltype(g_steam.GetHTTPResponseBodyData)>(g_steam.module, "SteamAPI_ISteamHTTP_GetHTTPResponseBodyData");
		g_steam.GetHTTPResponseHeaderSize =
			ResolveSymbol<decltype(g_steam.GetHTTPResponseHeaderSize)>(g_steam.module, "SteamAPI_ISteamHTTP_GetHTTPResponseHeaderSize");
		g_steam.GetHTTPResponseHeaderValue =
			ResolveSymbol<decltype(g_steam.GetHTTPResponseHeaderValue)>(g_steam.module, "SteamAPI_ISteamHTTP_GetHTTPResponseHeaderValue");
		g_steam.ReleaseHTTPRequest = ResolveSymbol<decltype(g_steam.ReleaseHTTPRequest)>(g_steam.module, "SteamAPI_ISteamHTTP_ReleaseHTTPRequest");
		g_steam.RegisterCallResult = ResolveSymbol<decltype(g_steam.RegisterCallResult)>(g_steam.module, "SteamAPI_RegisterCallResult");
		g_steam.UnregisterCallResult = ResolveSymbol<decltype(g_steam.UnregisterCallResult)>(g_steam.module, "SteamAPI_UnregisterCallResult");

		const bool complete = g_steam.getHttp && g_steam.CreateHTTPRequest && g_steam.SetHTTPRequestHeaderValue
							  && g_steam.SetHTTPRequestRawPostBody && g_steam.SendHTTPRequest
							  && g_steam.ReleaseHTTPRequest && g_steam.RegisterCallResult
							  && g_steam.UnregisterCallResult;
		if (!complete) {
			g_symbolFailure = true;
			g_loadWarningLogged = true;
			LogWarning("libsteam_api.so does not export the expected ISteamHTTP entry points; the Steam HTTP "
					   "transport stays disabled.");
			g_steam.getHttp = nullptr;
			return false;
		}
	}
	if (g_steam.http == nullptr) {
		g_steam.http = g_steam.getHttp();
	}
	return g_steam.http != nullptr;
}

// Full URLs must never reach the log: a Discord webhook URL *is* the credential (its token grants
// post/delete access), and S3 keys identify private objects. Keep scheme + host only.
std::string RedactedUrl(const std::string& url) {
	return k4::RedactUrl(url);
}

int32_t SteamMethod(const std::string& method) {
	if (method == "GET") return 1;
	if (method == "HEAD") return 2;
	if (method == "POST") return 3;
	if (method == "PUT") return 4;
	if (method == "DELETE") return 5;
	return 0;
}

} // namespace

struct HttpService::Impl {
	enum class ActiveTransport { None, Steam, Curl };

	struct CurlJob {
		HttpRequest request;
		std::string caFile;
	};

	// --- shared state (game thread) ------------------------------------------------------------
	HttpTransport mode = HttpTransport::Auto;
	std::string caFile;

	std::deque<HttpRequest> queue;
	HttpRequest active; // Steam request in flight (not used for curl)
	bool hasActive = false;
	ActiveTransport activeTransport = ActiveTransport::None;

	bool curlChecked = false;
	bool curlAvailable = false;
	bool curlMissingLogged = false;

	// --- Steam state (game thread; the call-result callback runs on the engine thread) ---------
	uint32_t request = 0;
	uint64_t call = 0;
	std::chrono::steady_clock::time_point started {};
	bool responseReady = false;
	bool responseFailed = true;
	bool responseIoFailure = false;
	bool responseRequestSuccessful = false;
	int responseStatus = 0;
	std::string responseBody;
	std::vector<std::pair<std::string, std::string>> responseHeaders;
	bool unavailableWarned = false;
	bool steamStatusWarningLogged = false;
	std::chrono::steady_clock::time_point steamRetryAt {};

	// --- curl state (the worker thread touches only these, under curlMutex) --------------------
	std::thread curlThread;
	std::mutex curlMutex;
	std::condition_variable curlCondition;
	std::deque<CurlJob> curlJobs;
	std::deque<std::pair<HttpRequest, CurlResponse>> curlDone;
	bool curlStopping = false;
	std::atomic<bool> curlAbort {false};

	bool CurlIsAvailable();
	void EnsureCurlThread();
	void CurlLoop();
	void StartCurl(HttpRequest request);
	bool StartSteam(HttpRequest request);
	void StartNext();
	void DeliverActive(HttpResponse response);
	void DeliverCurlCompletions();
	void CancelSteam();

	void CompleteResponse(bool failed, bool ioFailure, bool requestSuccessful, int status, std::string body,
						  std::vector<std::pair<std::string, std::string>> headers);
};

namespace {

class HttpCallback final : public CallbackBase {
public:
	explicit HttpCallback(HttpService::Impl* impl) : impl(impl) {
		m_nCallbackFlags = static_cast<uint8_t>(k_ECallbackFlagsGameServer);
		m_iCallback = HTTPRequestCompleted_t::k_iCallback;
	}

	void Run(void* pvParam) override { Run(pvParam, false, 0); }
	void Run(void* pvParam, bool bIOFailure, uint64_t /*hSteamAPICall*/) override {
		try {
			if (impl == nullptr) {
				return;
			}
			if (bIOFailure || pvParam == nullptr) {
				// The docs: m_bRequestSuccessful is false only for an internal error or a client
				// side network failure - no HTTP response was received.
				impl->CompleteResponse(true, true, false, 0, {}, {});
				return;
			}
			const auto* completed = static_cast<const HTTPRequestCompleted_t*>(pvParam);
			std::string body;
			if (completed->m_unBodySize > 0 && completed->m_unBodySize <= kMaximumResponseBody && g_steam.http
				&& g_steam.GetHTTPResponseBodySize && g_steam.GetHTTPResponseBodyData) {
				uint32_t size = 0;
				if (g_steam.GetHTTPResponseBodySize(g_steam.http, completed->m_hRequest, &size) && size > 0
					&& size <= kMaximumResponseBody) {
					body.resize(size);
					uint32_t received = size;
					if (!g_steam.GetHTTPResponseBodyData(g_steam.http, completed->m_hRequest,
														 reinterpret_cast<uint8_t*>(body.data()), received)) {
						body.clear();
					} else {
						body.resize(received);
					}
				}
			}

			std::vector<std::pair<std::string, std::string>> headers;
			if (g_steam.http && g_steam.GetHTTPResponseHeaderSize && g_steam.GetHTTPResponseHeaderValue) {
				for (const std::string& name : impl->active.captureHeaders) {
					uint32_t size = 0;
					if (!g_steam.GetHTTPResponseHeaderSize(g_steam.http, completed->m_hRequest, name.c_str(), &size)
						|| size == 0 || size > kMaximumHeaderValue) {
						continue;
					}
					std::string value(size, '\0');
					uint32_t received = size;
					if (!g_steam.GetHTTPResponseHeaderValue(g_steam.http, completed->m_hRequest, name.c_str(),
															reinterpret_cast<uint8_t*>(value.data()), received)) {
						continue;
					}
					value.resize(std::min<std::size_t>(received, value.size()));
					while (!value.empty() && (value.back() == '\0' || value.back() == '\r' || value.back() == '\n')) {
						value.pop_back();
					}
					headers.emplace_back(name, std::move(value));
				}
			}

			// m_bRequestSuccessful is documented as false only when no HTTP response was received,
			// but older Steam builds have been seen to clear it for error responses too. A status
			// code proves a response exists, so never hide it behind "transport failure".
			const int status = static_cast<int>(completed->m_eStatusCode);
			const bool gotResponse = completed->m_bRequestSuccessful || status >= 100;
			impl->CompleteResponse(!gotResponse, false, completed->m_bRequestSuccessful, status, std::move(body),
								   std::move(headers));
		} catch (...) {
			// Never unwind into Steam's callback pump.
		}
	}
	int GetCallbackSizeBytes() override { return static_cast<int>(sizeof(HTTPRequestCompleted_t)); }

	HttpService::Impl* impl {};
};

HttpCallback g_callback {nullptr};

} // namespace

bool HttpService::Impl::CurlIsAvailable() {
	if (!curlChecked) {
		curlChecked = true;
		std::string version;
		curlAvailable = CurlAvailable(&version);
		if (curlAvailable) {
			LogInfo("libcurl fallback transport available" + (version.empty() ? std::string() : " (" + version + ")"));
		} else if (!curlMissingLogged) {
			curlMissingLogged = true;
			LogInfo("libcurl is not available on this host; the Steam HTTP transport stays the only one.");
		}
	}
	return curlAvailable;
}

void HttpService::Impl::EnsureCurlThread() {
	if (curlThread.joinable()) {
		return;
	}
	curlStopping = false;
	curlAbort.store(false, std::memory_order_relaxed);
	curlThread = std::thread([this] { CurlLoop(); });
}

void HttpService::Impl::CurlLoop() {
	for (;;) {
		CurlJob job;
		{
			std::unique_lock<std::mutex> lock(curlMutex);
			curlCondition.wait(lock, [this] { return curlStopping || !curlJobs.empty(); });
			if (curlStopping && curlJobs.empty()) {
				return;
			}
			job = std::move(curlJobs.front());
			curlJobs.pop_front();
		}
		CurlResponse response;
		try {
			CurlRequest request;
			request.method = job.request.method;
			request.url = job.request.url;
			for (const HttpHeader& header : job.request.headers) {
				request.headers.push_back({header.name, header.value});
			}
			request.contentType = job.request.contentType;
			request.body = std::move(job.request.body);
			request.captureHeaders = job.request.captureHeaders;
			request.timeout = job.request.timeout;
			response = CurlPerform(request, job.caFile, &curlAbort);
		} catch (const std::exception& exception) {
			// Never let an exception escape the thread (std::terminate would take the server down).
			response = CurlResponse {};
			response.error = std::string("curl transport exception: ") + exception.what();
		} catch (...) {
			response = CurlResponse {};
			response.error = "curl transport exception";
		}
		{
			std::lock_guard<std::mutex> lock(curlMutex);
			curlDone.emplace_back(std::move(job.request), std::move(response));
		}
	}
}

void HttpService::Impl::StartCurl(HttpRequest next) {
	{
		std::lock_guard<std::mutex> lock(curlMutex);
		EnsureCurlThread();
		CurlJob job;
		job.request = std::move(next);
		job.caFile = caFile;
		curlJobs.push_back(std::move(job));
	}
	curlCondition.notify_one();
	hasActive = true;
	activeTransport = ActiveTransport::Curl;
}

void HttpService::Impl::CancelSteam() {
	// Unregister first: a pending call result would otherwise fire into a destroyed Impl.
	if (call != 0 && g_steam.UnregisterCallResult) {
		g_steam.UnregisterCallResult(&g_callback, call);
		call = 0;
	}
	if (request != 0 && g_steam.http && g_steam.ReleaseHTTPRequest) {
		g_steam.ReleaseHTTPRequest(g_steam.http, request);
	}
	request = 0;
	responseReady = false;
	g_callback.impl = nullptr;
}

void HttpService::Impl::CompleteResponse(bool failed, bool ioFailure, bool requestSuccessful, int status,
										 std::string body, std::vector<std::pair<std::string, std::string>> headers) {
	responseReady = true;
	responseFailed = failed;
	responseIoFailure = ioFailure;
	responseRequestSuccessful = requestSuccessful;
	responseStatus = status;
	responseBody = std::move(body);
	responseHeaders = std::move(headers);
}

void HttpService::Impl::DeliverActive(HttpResponse response) {
	HttpRequest finished = std::move(active);
	active = {};
	hasActive = false;
	activeTransport = ActiveTransport::None;
	// A request is attempted exactly once: the live server showed ISteamHTTP delivering the PUT
	// and then losing the response, so replaying it (over any transport) would upload the object
	// twice. The transport choice is made once, in StartNext().
	if (finished.onDone) {
		try {
			finished.onDone(std::move(response));
		} catch (...) {
			// A callback must never unwind into the engine's event/update path.
		}
	}
}

void HttpService::Impl::DeliverCurlCompletions() {
	std::deque<std::pair<HttpRequest, CurlResponse>> done;
	{
		std::lock_guard<std::mutex> lock(curlMutex);
		done.swap(curlDone);
	}
	for (auto& [finished, curlResponse] : done) {
		hasActive = false;
		activeTransport = ActiveTransport::None;
		HttpResponse response;
		response.transport = "curl";
		response.transportFailed = curlResponse.transportFailed;
		response.status = curlResponse.status;
		response.body = std::move(curlResponse.body);
		response.headers = std::move(curlResponse.headers);
		response.transportError = curlResponse.error;
		if (finished.onDone) {
			try {
				finished.onDone(std::move(response));
			} catch (...) {
				// See DeliverActive(): callbacks stay inside the HTTP layer's pump.
			}
		}
	}
}

bool HttpService::Impl::StartSteam(HttpRequest next) {
	active = std::move(next);
	activeTransport = ActiveTransport::Steam;
	hasActive = true;

	const int32_t method = SteamMethod(active.method);
	if (method == 0) {
		LogError("HTTP request failed: unsupported method '" + active.method + "'");
		HttpResponse response;
		response.transport = "steam";
		response.transportError = "unsupported method '" + active.method + "'";
		DeliverActive(std::move(response));
		return true;
	}
	request = g_steam.CreateHTTPRequest(g_steam.http, method, active.url.c_str());
	if (request == 0) {
		LogError("Steam HTTP could not create a request for " + active.method + " " + RedactedUrl(active.url));
		HttpResponse response;
		response.transport = "steam";
		response.transportError = "Steam HTTP could not create a request";
		DeliverActive(std::move(response));
		return true;
	}
	{
		// Steam's documented default inactivity timeout is 60 s; a multi-MB upload on a slow link
		// must get the request's own budget (the functions exist only in newer SDK builds).
		const uint64_t timeoutMs = static_cast<uint64_t>(std::max<long long>(0, active.timeout.count()));
		const uint32_t seconds = static_cast<uint32_t>(std::min<uint64_t>(std::max<uint64_t>(60, timeoutMs / 1000), 86400));
		if (g_steam.SetHTTPRequestNetworkActivityTimeout) {
			g_steam.SetHTTPRequestNetworkActivityTimeout(g_steam.http, request, seconds);
		}
		if (g_steam.SetHTTPRequestAbsoluteTimeoutMS && timeoutMs > 0) {
			g_steam.SetHTTPRequestAbsoluteTimeoutMS(
				g_steam.http, request, static_cast<uint32_t>(std::min<uint64_t>(timeoutMs, 0xffffffffu)));
		}
	}

	// Only the headers the SigV4 signer signed are set. Host, Content-Length and Transfer-Encoding
	// belong to Steam's HTTP client (it derives them from the URL and the body); setting them
	// manually is outside ISteamHTTP's header contract and a different Host value would also break
	// the signature. The curl transport does the same, plus an explicit "Expect:" (no
	// 100-continue wait) which ISteamHTTP has no knob for.
	for (const HttpHeader& header : active.headers) {
		if (!g_steam.SetHTTPRequestHeaderValue(g_steam.http, request, header.name.c_str(), header.value.c_str())) {
			// Never log the values: they carry the Authorization header.
			LogError("Steam HTTP rejected the header '" + header.name + "' for " + active.method + " "
					 + RedactedUrl(active.url));
			g_steam.ReleaseHTTPRequest(g_steam.http, request);
			request = 0;
			HttpResponse response;
			response.transport = "steam";
			response.transportError = "Steam HTTP rejected the request header '" + header.name + "'";
			DeliverActive(std::move(response));
			return true;
		}
	}

	if (!active.body.empty()) {
		if (active.body.size() > std::numeric_limits<uint32_t>::max()) {
			LogError("HTTP request body is too large for Steam HTTP");
			g_steam.ReleaseHTTPRequest(g_steam.http, request);
			request = 0;
			HttpResponse response;
			response.transport = "steam";
			response.transportError = "request body too large for Steam HTTP";
			DeliverActive(std::move(response));
			return true;
		}
		const std::string contentType = active.contentType.empty() ? "application/octet-stream" : active.contentType;
		if (!g_steam.SetHTTPRequestRawPostBody(g_steam.http, request, contentType.c_str(),
											   reinterpret_cast<uint8_t*>(active.body.data()),
											   static_cast<uint32_t>(active.body.size()))) {
			LogError("Steam HTTP rejected the " + std::to_string(active.body.size()) + "-byte body for "
					 + active.method + " " + RedactedUrl(active.url));
			g_steam.ReleaseHTTPRequest(g_steam.http, request);
			request = 0;
			HttpResponse response;
			response.transport = "steam";
			response.transportError = "Steam HTTP rejected the request body";
			DeliverActive(std::move(response));
			return true;
		}
	}

	uint64_t callId = 0;
	if (!g_steam.SendHTTPRequest(g_steam.http, request, &callId)) {
		LogError("Steam HTTP could not send " + active.method + " " + RedactedUrl(active.url));
		g_steam.ReleaseHTTPRequest(g_steam.http, request);
		request = 0;
		HttpResponse response;
		response.transport = "steam";
		response.transportError = "Steam HTTP could not send the request";
		DeliverActive(std::move(response));
		return true;
	}
	started = std::chrono::steady_clock::now();
	responseReady = false;
	g_callback.impl = this;
	call = callId;
	g_steam.RegisterCallResult(&g_callback, callId);
	return true;
}

void HttpService::Impl::StartNext() {
	if (hasActive || queue.empty()) {
		return;
	}
	const bool curlReady = CurlIsAvailable();
	bool useCurl = false;
	switch (mode) {
	case HttpTransport::Curl:
		useCurl = curlReady;
		break;
	case HttpTransport::Steam:
		useCurl = false;
		break;
	case HttpTransport::Auto:
		// Default: libcurl whenever it loads (Steam loses the response of a delivered PUT);
		// ISteamHTTP only when libcurl is unavailable on this host.
		useCurl = curlReady;
		break;
	}
	if (useCurl) {
		HttpRequest next = std::move(queue.front());
		queue.pop_front();
		StartCurl(std::move(next));
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	if (now < steamRetryAt) {
		return;
	}
	if (!ResolveSteamHttp()) {
		if (!unavailableWarned) {
			unavailableWarned = true;
			LogWarning("Steam's HTTP service is not ready; queued S3/Discord requests wait.");
		}
		steamRetryAt = now + std::chrono::seconds(5); // do not dlopen once per frame
		return;
	}
	unavailableWarned = false;
	steamRetryAt = {};
	HttpRequest next = std::move(queue.front());
	queue.pop_front();
	StartSteam(std::move(next));
}

HttpService::HttpService() : m_impl(std::make_unique<Impl>()) {}
HttpService::~HttpService() {
	Shutdown();
}

void HttpService::Configure(HttpTransport transport, std::string caFile) {
	m_impl->mode = transport;
	m_impl->caFile = std::move(caFile);
	// Probe/report the fallback once at startup so the live log shows what the host offers.
	m_impl->CurlIsAvailable();
}

void HttpService::Enqueue(HttpRequest request) {
	if (request.onDone == nullptr) {
		return;
	}
	m_impl->queue.push_back(std::move(request));
}

void HttpService::Pump() {
	Impl& impl = *m_impl;
	impl.DeliverCurlCompletions();

	if (impl.hasActive && impl.activeTransport == Impl::ActiveTransport::Steam) {
		const auto now = std::chrono::steady_clock::now();
		if (impl.request != 0 && !impl.responseReady && now - impl.started > impl.active.timeout) {
			impl.CancelSteam();
			HttpResponse response;
			response.transport = "steam";
			response.transportError = "the Steam HTTP request timed out after "
									  + std::to_string(impl.active.timeout.count()) + " ms";
			impl.DeliverActive(std::move(response));
		} else if (impl.responseReady) {
			HttpResponse response;
			response.transport = "steam";
			response.transportFailed = impl.responseFailed;
			response.ioFailure = impl.responseIoFailure;
			response.requestSuccessful = impl.responseRequestSuccessful;
			response.status = impl.responseStatus;
			response.body = std::move(impl.responseBody);
			response.headers = std::move(impl.responseHeaders);
			impl.responseBody.clear();
			impl.responseHeaders.clear();
			impl.responseReady = false;
			if (response.transportFailed) {
				response.transportError = "no HTTP response (io failure "
										  + std::to_string(response.ioFailure ? 1 : 0) + ", request successful "
										  + std::to_string(response.requestSuccessful ? 1 : 0) + ", status "
										  + std::to_string(response.status) + ")";
			} else if (!response.requestSuccessful && !impl.steamStatusWarningLogged) {
				// A status code with m_bRequestSuccessful=0: some Steam builds report error
				// responses that way. The response is real; say so once (the S3 log shows it).
				impl.steamStatusWarningLogged = true;
				LogWarning("Steam HTTP returned HTTP " + std::to_string(response.status)
						   + " with requestSuccessful=0; treating it as an HTTP response.");
			}
			if (impl.request != 0 && g_steam.http && g_steam.ReleaseHTTPRequest) {
				g_steam.ReleaseHTTPRequest(g_steam.http, impl.request);
			}
			impl.request = 0;
			// Steam unregisters a call result once it has fired; the id is only kept so an
			// outstanding request can be cancelled on unload.
			impl.call = 0;
			g_callback.impl = nullptr;
			impl.DeliverActive(std::move(response));
		}
	}

	impl.StartNext();
}

void HttpService::Shutdown() {
	Impl& impl = *m_impl;
	impl.CancelSteam();
	impl.queue.clear();
	impl.active = {};
	impl.hasActive = false;
	impl.activeTransport = Impl::ActiveTransport::None;
	impl.responseBody.clear();
	impl.responseHeaders.clear();
	impl.responseReady = false;
	impl.unavailableWarned = false;
	impl.steamRetryAt = {};
	impl.curlAbort.store(true, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(impl.curlMutex);
		impl.curlStopping = true;
		impl.curlJobs.clear();
	}
	impl.curlCondition.notify_all();
	if (impl.curlThread.joinable()) {
		impl.curlThread.join(); // the progress callback aborts the transfer within ~1 s
	}
	{
		std::lock_guard<std::mutex> lock(impl.curlMutex);
		impl.curlDone.clear();
	}
}

bool HttpService::Busy() const {
	return m_impl->hasActive || !m_impl->queue.empty();
}

std::size_t HttpService::Pending() const {
	std::size_t pending = m_impl->queue.size() + (m_impl->hasActive ? 1u : 0u);
	std::lock_guard<std::mutex> lock(m_impl->curlMutex);
	pending += m_impl->curlDone.size() + m_impl->curlJobs.size();
	return pending;
}

} // namespace k4
