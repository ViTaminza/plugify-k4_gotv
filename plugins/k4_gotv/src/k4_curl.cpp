// k4_gotv — dlopen'ed libcurl transport (see k4_curl.hpp).
//
// Only the stable C ABI is used. The option numbers are the public values from curl's easy.h
// (unchanged for years); the runtime library is looked up by soname, so the plugin keeps zero
// DT_NEEDED entries for it and works on hosts where it is absent (CurlAvailable() == false).

#include "k4_curl.hpp"

#include <dlfcn.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace k4 {
namespace {

// --- curl ABI (easy.h) -------------------------------------------------------------------------
struct CurlHandle; // CURL
struct CurlSlist;  // struct curl_slist

// CURLoption values used here (curl/easy.h, stable ABI).
constexpr int kOptWriteData = 10001;
constexpr int kOptUrl = 10002;
constexpr int kOptErrorBuffer = 10010;
constexpr int kOptPostFields = 10015;
constexpr int kOptHttpHeader = 10023;
constexpr int kOptHeaderData = 10029;
constexpr int kOptCustomRequest = 10036;
constexpr int kOptNoBody = 44;
constexpr int kOptFollowLocation = 52;
constexpr int kOptSslVerifyPeer = 64;
constexpr int kOptCaInfo = 10065;
constexpr int kOptSslVerifyHost = 81;
constexpr int kOptHttpVersion = 84;
constexpr int kOptNoSignal = 99;
constexpr int kOptTimeoutMs = 155;
constexpr int kOptConnectTimeoutMs = 156;
constexpr int kOptProtocols = 181;
constexpr int kOptExpect100TimeoutMs = 227;
constexpr int kOptWriteFunction = 20011;
constexpr int kOptHeaderFunction = 20079;
constexpr int kOptNoProgress = 43;
constexpr int kOptXferInfoFunction = 20219;
constexpr int kOptXferInfoData = 10057;
constexpr int kOptPostFieldSizeLarge = 30120;

constexpr int kInfoResponseCode = 0x200002;

constexpr int kHttpVersion11 = 2; // CURL_HTTP_VERSION_1_1
constexpr long kProtoHttps = 2;	  // CURLPROTO_HTTPS

constexpr int kOk = 0; // CURLE_OK

constexpr std::size_t kErrorBufferSize = 256; // CURL_ERROR_SIZE
constexpr std::size_t kMaximumBody = 4u * 1024u * 1024u;
constexpr std::size_t kMaximumHeaderBytes = 256u * 1024u;
constexpr std::size_t kMaximumHeaderValue = 64u * 1024u;

using CurlInit = CurlHandle* (*)();
using CurlCleanup = void (*)(CurlHandle*);
using CurlSetopt = int (*)(CurlHandle*, int, ...);
using CurlPerform = int (*)(CurlHandle*);
using CurlGetinfo = int (*)(CurlHandle*, int, ...);
using CurlStrError = const char* (*)(int);
using CurlSlistAppend = CurlSlist* (*)(CurlSlist*, const char*);
using CurlSlistFreeAll = void (*)(CurlSlist*);
using CurlGlobalInit = int (*)(long);
using CurlVersion = const char* (*)();

struct CurlApi {
	void* module = nullptr;
	CurlInit init = nullptr;
	CurlCleanup cleanup = nullptr;
	CurlSetopt setopt = nullptr;
	CurlPerform perform = nullptr;
	CurlGetinfo getinfo = nullptr;
	CurlStrError strerror = nullptr;
	CurlSlistAppend slistAppend = nullptr;
	CurlSlistFreeAll slistFreeAll = nullptr;
	CurlVersion version = nullptr;
	std::string versionText;
	bool resolved = false;
	bool available = false;
};

CurlApi& Api() {
	static CurlApi api;
	return api;
}

std::mutex& LoaderMutex() {
	static std::mutex mutex;
	return mutex;
}

// Caller holds LoaderMutex().
void LoadCurl(CurlApi& api) {
	if (api.resolved) {
		return;
	}
	api.resolved = true;
	const char* candidates[] = {
		"libcurl-gnutls.so.4",
		"libcurl.so.4",
		"libcurl.so",
		"/usr/lib/x86_64-linux-gnu/libcurl-gnutls.so.4",
		"/usr/lib/x86_64-linux-gnu/libcurl.so.4",
		"/usr/lib64/libcurl-gnutls.so.4",
		"/usr/lib64/libcurl.so.4",
		"/lib/x86_64-linux-gnu/libcurl-gnutls.so.4",
		"/lib/x86_64-linux-gnu/libcurl.so.4",
	};
	for (const char* candidate : candidates) {
		// RTLD_LOCAL: never interpose the game's own libcurl/OpenSSL symbols.
		if (void* handle = dlopen(candidate, RTLD_NOW | RTLD_LOCAL)) {
			api.module = handle;
			break;
		}
	}
	if (api.module == nullptr) {
		return;
	}
	auto symbol = [&api](const char* name) { return dlsym(api.module, name); };
	api.init = reinterpret_cast<CurlInit>(symbol("curl_easy_init"));
	api.cleanup = reinterpret_cast<CurlCleanup>(symbol("curl_easy_cleanup"));
	api.setopt = reinterpret_cast<CurlSetopt>(symbol("curl_easy_setopt"));
	api.perform = reinterpret_cast<CurlPerform>(symbol("curl_easy_perform"));
	api.getinfo = reinterpret_cast<CurlGetinfo>(symbol("curl_easy_getinfo"));
	api.strerror = reinterpret_cast<CurlStrError>(symbol("curl_easy_strerror"));
	api.slistAppend = reinterpret_cast<CurlSlistAppend>(symbol("curl_slist_append"));
	api.slistFreeAll = reinterpret_cast<CurlSlistFreeAll>(symbol("curl_slist_free_all"));
	api.version = reinterpret_cast<CurlVersion>(symbol("curl_version"));
	auto globalInit = reinterpret_cast<CurlGlobalInit>(symbol("curl_global_init"));
	if (api.init == nullptr || api.cleanup == nullptr || api.setopt == nullptr || api.perform == nullptr
		|| api.getinfo == nullptr || api.strerror == nullptr || api.slistAppend == nullptr
		|| api.slistFreeAll == nullptr) {
		dlclose(api.module);
		api.module = nullptr;
		return;
	}
	if (globalInit != nullptr) {
		globalInit(3); // CURL_GLOBAL_DEFAULT
	}
	if (api.version != nullptr) {
		const char* text = api.version();
		if (text != nullptr) {
			api.versionText = text;
		}
	}
	api.available = true;
}

struct HeaderSink {
	std::vector<std::pair<std::string, std::string>> captured;
	std::vector<std::string> wanted;
	std::size_t totalBytes = 0;
	std::string pending;
};

bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.size(); ++index) {
		const auto a = static_cast<unsigned char>(left[index]);
		const auto b = static_cast<unsigned char>(right[index]);
		const auto lower = [](unsigned char c) {
			return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c - 'A' + 'a') : c;
		};
		if (lower(a) != lower(b)) {
			return false;
		}
	}
	return true;
}

std::string TrimHeaderValue(std::string value) {
	while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
		value.pop_back();
	}
	std::size_t first = 0;
	while (first < value.size() && (value[first] == ' ' || value[first] == '\t')) {
		++first;
	}
	return value.substr(first);
}

// One header line per call in practice, but keep a small buffer so a split line still works. A
// new "HTTP/..." status line starts a new response (the 100 Continue intermediate included).
void HandleHeaderLine(HeaderSink& sink, const std::string& line) {
	std::string_view view(line);
	if (view.rfind("HTTP/", 0) == 0) {
		sink.captured.clear();
		return;
	}
	if (!view.empty() && view.back() == '\r') {
		view.remove_suffix(1);
	}
	if (view.empty()) {
		return;
	}
	const std::size_t colon = view.find(':');
	if (colon == std::string_view::npos) {
		return;
	}
	const std::string_view name = view.substr(0, colon);
	for (const std::string& wanted : sink.wanted) {
		if (!EqualsIgnoreCase(name, wanted)) {
			continue;
		}
		const std::string value = TrimHeaderValue(std::string(view.substr(colon + 1)));
		if (value.size() > kMaximumHeaderValue) {
			return;
		}
		for (auto& entry : sink.captured) {
			if (EqualsIgnoreCase(entry.first, wanted)) {
				entry.second = value;
				return;
			}
		}
		sink.captured.emplace_back(wanted, value);
		return;
	}
}

std::size_t HeaderCallback(char* buffer, std::size_t size, std::size_t count, void* userdata) {
	const std::size_t bytes = size * count;
	auto* sink = static_cast<HeaderSink*>(userdata);
	if (sink == nullptr) {
		return bytes;
	}
	sink->totalBytes += bytes;
	if (sink->totalBytes > kMaximumHeaderBytes) {
		return 0; // abort: an endless header stream is not a valid response
	}
	sink->pending.append(buffer, bytes);
	for (;;) {
		const std::size_t newline = sink->pending.find('\n');
		if (newline == std::string::npos) {
			break;
		}
		const std::string line = sink->pending.substr(0, newline);
		sink->pending.erase(0, newline + 1);
		HandleHeaderLine(*sink, line);
	}
	return bytes;
}

struct BodySink {
	std::string data;
};

std::size_t WriteCallback(char* buffer, std::size_t size, std::size_t count, void* userdata) {
	const std::size_t bytes = size * count;
	auto* sink = static_cast<BodySink*>(userdata);
	if (sink == nullptr) {
		return bytes;
	}
	if (sink->data.size() < kMaximumBody) {
		const std::size_t room = kMaximumBody - sink->data.size();
		sink->data.append(buffer, bytes < room ? bytes : room);
	}
	return bytes;
}

struct ProgressSink {
	const std::atomic<bool>* abort = nullptr;
};

int ProgressCallback(void* userdata, long long /*dltotal*/, long long /*dlnow*/, long long /*ultotal*/, long long /*ulnow*/) {
	auto* sink = static_cast<ProgressSink*>(userdata);
	if (sink != nullptr && sink->abort != nullptr && sink->abort->load(std::memory_order_relaxed)) {
		return 1; // abort
	}
	return 0;
}

bool MethodAllowsBody(const std::string& method) {
	return method == "POST" || method == "PUT" || method == "PATCH";
}

} // namespace

bool CurlAvailable(std::string* version) {
	std::lock_guard<std::mutex> lock(LoaderMutex());
	CurlApi& api = Api();
	LoadCurl(api);
	if (version != nullptr) {
		*version = api.versionText;
	}
	return api.available;
}

CurlResponse CurlPerform(const CurlRequest& request, const std::string& caFile, const std::atomic<bool>* abort) {
	CurlResponse response;
	CurlApi& api = Api();
	{
		std::lock_guard<std::mutex> lock(LoaderMutex());
		LoadCurl(api);
	}
	if (!api.available) {
		response.error = "libcurl is not available";
		return response;
	}
	if (request.url.rfind("https://", 0) != 0) {
		response.error = "only https:// URLs are allowed";
		return response;
	}
	if (request.method.empty()) {
		response.error = "empty HTTP method";
		return response;
	}

	CurlHandle* handle = api.init();
	if (handle == nullptr) {
		response.error = "curl_easy_init failed";
		return response;
	}

	CurlSlist* headers = nullptr;
	bool headersOk = true;
	auto appendHeader = [&](const std::string& line) {
		CurlSlist* appended = api.slistAppend(headers, line.c_str());
		if (appended == nullptr) {
			headersOk = false;
		} else {
			headers = appended;
		}
	};
	appendHeader("Expect:"); // never send/wait for 100-continue
	for (const CurlHeader& header : request.headers) {
		appendHeader(header.name + ": " + header.value);
	}
	if (!request.body.empty() && !request.contentType.empty()) {
		appendHeader("Content-Type: " + request.contentType);
	}
	if (!headersOk) {
		api.slistFreeAll(headers);
		api.cleanup(handle);
		response.error = "out of memory building the request headers";
		return response;
	}

	HeaderSink headerSink;
	for (const std::string& wanted : request.captureHeaders) {
		if (headerSink.wanted.size() >= 64) {
			break;
		}
		headerSink.wanted.push_back(wanted);
	}
	BodySink bodySink;
	ProgressSink progressSink {abort};
	char errorBuffer[kErrorBufferSize] {};
	errorBuffer[0] = '\0';

	api.setopt(handle, kOptUrl, request.url.c_str());
	api.setopt(handle, kOptCustomRequest, request.method.c_str());
	api.setopt(handle, kOptErrorBuffer, errorBuffer);
	api.setopt(handle, kOptHttpHeader, headers);
	api.setopt(handle, kOptWriteFunction, &WriteCallback);
	api.setopt(handle, kOptWriteData, &bodySink);
	api.setopt(handle, kOptHeaderFunction, &HeaderCallback);
	api.setopt(handle, kOptHeaderData, &headerSink);
	api.setopt(handle, kOptNoSignal, 1L);
	api.setopt(handle, kOptFollowLocation, 0L);
	api.setopt(handle, kOptSslVerifyPeer, 1L);
	api.setopt(handle, kOptSslVerifyHost, 2L);
	api.setopt(handle, kOptHttpVersion, static_cast<long>(kHttpVersion11));
	api.setopt(handle, kOptProtocols, kProtoHttps);
	api.setopt(handle, kOptExpect100TimeoutMs, 0L);
	api.setopt(handle, kOptConnectTimeoutMs, 15000L);
	const long timeoutMs = static_cast<long>(request.timeout.count());
	if (timeoutMs > 0) {
		api.setopt(handle, kOptTimeoutMs, timeoutMs);
	}
	api.setopt(handle, kOptNoProgress, 0L);
	api.setopt(handle, kOptXferInfoFunction, &ProgressCallback);
	api.setopt(handle, kOptXferInfoData, &progressSink);
	if (!caFile.empty()) {
		api.setopt(handle, kOptCaInfo, caFile.c_str());
	}
	if (request.method == "HEAD") {
		api.setopt(handle, kOptNoBody, 1L);
	}
	if (MethodAllowsBody(request.method) || !request.body.empty()) {
		api.setopt(handle, kOptPostFields, const_cast<char*>(request.body.c_str()));
		api.setopt(handle, kOptPostFieldSizeLarge, static_cast<long long>(request.body.size()));
	}

	const int result = api.perform(handle);
	if (result != kOk) {
		if (errorBuffer[0] != '\0') {
			response.error = "curl error " + std::to_string(result) + ": " + std::string(errorBuffer);
		} else {
			const char* text = api.strerror(result);
			response.error = "curl error " + std::to_string(result) + ": " + (text != nullptr ? std::string(text) : std::string("unknown"));
		}
		api.slistFreeAll(headers);
		api.cleanup(handle);
		return response;
	}

	long status = 0;
	api.getinfo(handle, kInfoResponseCode, &status);
	response.status = static_cast<int>(status);
	response.transportFailed = false;
	response.body = std::move(bodySink.data);
	response.headers = std::move(headerSink.captured);
	api.slistFreeAll(headers);
	api.cleanup(handle);
	return response;
}

} // namespace k4
