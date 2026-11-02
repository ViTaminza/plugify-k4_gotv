// k4_gotv — engine-free test of the HTTP service's curl transport and completion plumbing.
//
// No network success is required: every request targets a reserved .invalid host, so libcurl
// fails at name resolution within milliseconds and the test only exercises the thread, the
// completion queue, the game-thread callback delivery and Shutdown. The Steam transport is not
// usable in this process (no libsteam_api.so), which is exactly the "Steam unavailable" case the
// auto mode has to route to libcurl.

#include "k4_curl.hpp"
#include "k4_http.hpp"

#include <plg/plugin.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace plg {
GetBaseDirFn GetBaseDir {nullptr};
GetExtensionsDirFn GetExtensionsDir {nullptr};
GetConfigsDirFn GetConfigsDir {nullptr};
GetDataDirFn GetDataDir {nullptr};
GetLogsDirFn GetLogsDir {nullptr};
GetCacheDirFn GetCacheDir {nullptr};
IsLoadedFn IsLoaded {nullptr};
LogFn Log {nullptr};
BeginZoneFn BeginZone {nullptr};
EndZoneFn EndZone {nullptr};
} // namespace plg

using k4::HttpService;

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
	if (!condition) {
		++g_failures;
		std::cerr << "CHECK failed: " << what << "\n";
	}
}

// Pump the service until the request completes or the deadline passes.
bool WaitFor(HttpService& service, bool* done) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!*done && std::chrono::steady_clock::now() < deadline) {
		service.Pump();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	service.Pump();
	return *done;
}

k4::HttpRequest MakeRequest(const std::string& method, const std::string& url, std::string body,
							std::function<void(k4::HttpResponse)> onDone) {
	k4::HttpRequest request;
	request.method = method;
	request.url = url;
	request.contentType = "application/zip";
	request.body = std::move(body);
	request.timeout = std::chrono::seconds(5);
	request.onDone = std::move(onDone);
	return request;
}

} // namespace

int RunHttpTests() {
	std::string curlVersion;
	if (!k4::CurlAvailable(&curlVersion)) {
		std::cout << "libcurl is not available; HTTP service tests skipped\n";
		return 0;
	}

	{
		// Curl mode: the request runs on the service's thread and the callback is delivered from
		// Pump() on this (game) thread.
		HttpService service;
		service.Configure(k4::HttpTransport::Curl, "");
		bool done = false;
		bool failed = false;
		std::string transport;
		std::string error;
		service.Enqueue(MakeRequest("PUT", "https://k4-gotv.invalid/probe.bin", "hello",
									[&](k4::HttpResponse response) {
										done = true;
										failed = response.transportFailed;
										transport = response.transport;
										error = response.transportError;
									}));
		Check(WaitFor(service, &done), "curl-mode request completed");
		Check(failed, "reserved .invalid host fails at the transport level");
		Check(transport == "curl", "response came from the curl transport");
		Check(error.find("curl error") != std::string::npos, "curl error text is reported");
		Check(!service.Busy(), "service is idle after the completion");
		service.Shutdown();
	}

	{
		// Auto mode: libcurl is the default transport whenever it loads (Steam is only the
		// fallback when libcurl is unavailable), so this request must go over curl.
		HttpService service;
		service.Configure(k4::HttpTransport::Auto, "");
		bool done = false;
		std::string transport;
		service.Enqueue(MakeRequest("PUT", "https://k4-gotv.invalid/probe.bin", "hello",
									[&](k4::HttpResponse response) {
										done = true;
										transport = response.transport;
									}));
		Check(WaitFor(service, &done), "auto-mode request completed");
		Check(transport == "curl", "auto mode prefers curl whenever it loads");
		service.Shutdown();
	}

	{
		// Queue order: two requests complete in FIFO order.
		HttpService service;
		service.Configure(k4::HttpTransport::Curl, "");
		std::string order;
		bool first = false;
		bool second = false;
		service.Enqueue(MakeRequest("PUT", "https://k4-gotv.invalid/a.bin", "a", [&](k4::HttpResponse) {
			order += '1';
			first = true;
		}));
		service.Enqueue(MakeRequest("PUT", "https://k4-gotv.invalid/b.bin", "b", [&](k4::HttpResponse) {
			order += '2';
			second = true;
		}));
		Check(WaitFor(service, &first), "first queued request completed");
		Check(WaitFor(service, &second), "second queued request completed");
		Check(order == "12", "requests are delivered in order");
		service.Shutdown();
	}

	{
		// Shutdown with an in-flight request returns promptly and drops the callback.
		HttpService service;
		service.Configure(k4::HttpTransport::Curl, "");
		bool called = false;
		service.Enqueue(MakeRequest("PUT", "https://k4-gotv.invalid/slow.bin", "x",
									[&](k4::HttpResponse) { called = true; }));
		service.Pump(); // start it
		const auto started = std::chrono::steady_clock::now();
		service.Shutdown();
		const auto elapsed = std::chrono::steady_clock::now() - started;
		Check(elapsed < std::chrono::seconds(5), "shutdown does not hang");
		service.Pump();
		Check(!called, "shutdown does not deliver the cancelled callback");
	}

	std::cout << "HTTP service failures: " << g_failures << '\n';
	return g_failures;
}

int main() {
	return RunHttpTests() == 0 ? 0 : 1;
}
