// Exercise the real S3 state machine with an in-memory HTTP transport (no network/engine).
#include "k4_crypto.hpp"
#include "k4_http.hpp"
#include "k4_s3.hpp"
#include "k4_worker.hpp"
#include <plg/plugin.hpp>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace plg { LogFn Log = nullptr; }
namespace {
std::deque<k4::HttpRequest> requests;
int failures = 0;
void Check(bool ok, const char* message) {
	if (!ok) { ++failures; std::cerr << "S3 state test failed: " << message << '\n'; }
}
}
namespace k4 {
struct HttpService::Impl {};
HttpService::HttpService() : m_impl(std::make_unique<Impl>()) {}
HttpService::~HttpService() { Shutdown(); }
void HttpService::Enqueue(HttpRequest request) { requests.push_back(std::move(request)); }
void HttpService::Shutdown() { requests.clear(); }
}
namespace {
k4::HttpRequest Next(k4::S3Client& client, k4::Worker& worker) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (requests.empty() && std::chrono::steady_clock::now() < deadline) {
		worker.Poll(); client.Pump();
		if (requests.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
	}
	if (requests.empty()) { Check(false, "request timed out"); return {}; }
	auto request = std::move(requests.front()); requests.pop_front(); return request;
}
void Respond(k4::HttpRequest& request, int status, std::string body = {},
			 std::vector<std::pair<std::string, std::string>> headers = {}) {
	if (request.onDone) {
		k4::HttpResponse response;
		response.status = status;
		response.transportFailed = false;
		response.body = std::move(body);
		response.headers = std::move(headers);
		response.transport = "steam";
		response.requestSuccessful = true;
		request.onDone(std::move(response));
	}
}
}
int RunS3StateTests() {
	k4::HttpService http;
	k4::Worker worker;
	k4::S3Client client;
	k4::S3Settings settings;
	settings.endpoint = "https://s3.example.invalid"; settings.bucket = "test";
	// Deliberately synthetic, non-secret signing inputs.
	settings.accessKey = "test"; settings.secretKey = "test"; settings.partSizeMb = 5;
	client.Configure(settings, &http, &worker);
	bool retained = false;
	client.RetentionScan(std::time(nullptr), 3, [&](int count, std::string error) {
		retained = true; Check(count == 0 && error.empty(), "retention result");
	});
	auto list = Next(client, worker);
	Check(list.method == "GET" && list.url.find("list-type=2") != std::string::npos, "queued retention starts");
	Respond(list, 200, "<ListBucketResult><IsTruncated>false</IsTruncated></ListBucketResult>");
	client.Pump();
	Check(retained && !client.Pending(), "retention completes and releases pending state");

	const auto path = std::filesystem::temp_directory_path() / "k4_gotv_state_test.zip";
	constexpr std::size_t size = 5 * 1024 * 1024 + 1;
	{ std::ofstream file(path, std::ios::binary); file.seekp(size - 1); file.put('\0'); }
	for (int scenario = 0; scenario < 3; ++scenario) {
		bool done = false; bool success = false;
		client.UploadFile(path.string(), "reused.zip", "application/zip", [&](k4::UploadOutcome result) {
			done = true; success = result.success;
		});
		auto create = Next(client, worker);
		std::string identity;
		for (const auto& header : create.headers) {
			if (header.name == "x-amz-meta-k4-upload-id") { identity = header.value; }
		}
		Check(!identity.empty(), "multipart creation carries upload identity");
		bool signedIdentity = false;
		for (const auto& header : create.headers) {
			if (header.name == "Authorization" && header.value.find("x-amz-meta-k4-upload-id") != std::string::npos) {
				signedIdentity = true;
			}
		}
		Check(signedIdentity, "upload identity is signed");
		Respond(create, 200, "<InitiateMultipartUploadResult><UploadId>test-id</UploadId></InitiateMultipartUploadResult>");
		for (int part = 1; part <= 2; ++part) {
			auto put = Next(client, worker);
			Check(put.method == "PUT" && put.url.find("partNumber=" + std::to_string(part)) != std::string::npos,
				  "ordered multipart part");
			Check(put.body.size() == (part == 1 ? size - 1 : 1), "part size");
			// The uploader signs the hash it computed on the file worker; it must be the hash of
			// exactly the body that is sent.
			bool hashMatches = false;
			for (const auto& header : put.headers) {
				if (header.name == "x-amz-content-sha256" && header.value == k4::Sha256Hex(put.body)) {
					hashMatches = true;
				}
			}
			Check(hashMatches, "signed payload hash matches the part body");
			if (scenario == 1 && part == 1) {
				Respond(put, 503);
				// The body was moved into the failed request; the retry must re-read the same part
				// on the worker instead of sending an empty one.
				put = Next(client, worker);
				Check(put.method == "PUT" && put.body.size() == size - 1, "failed part is re-read and retried");
			}
			Respond(put, 200, {}, {{"ETag", "\"part-etag\""}});
		}
		auto complete = Next(client, worker);
		Check(complete.method == "POST", "complete multipart");
		if (scenario == 2) {
			Respond(complete, 200, "<Error><Code>InternalError</Code></Error>");
			complete = Next(client, worker);
			Check(complete.method == "POST", "HTTP-200 InternalError retries completion");
			Respond(complete, 200, "<CompleteMultipartUploadResult/>");
		} else {
			Respond(complete, 404, "<Error><Code>NoSuchUpload</Code></Error>");
			auto head = Next(client, worker);
			Check(head.method == "HEAD", "NoSuchUpload probes object");
			Respond(head, 200, {}, {{"x-amz-meta-k4-upload-id", scenario == 0 ? "older-upload" : identity},
				{"Content-Length", std::to_string(size)}});
			if (scenario == 0) {
				auto abort = Next(client, worker);
				Check(abort.method == "DELETE", "wrong object identity aborts rather than succeeds");
				Respond(abort, 503);
				abort = Next(client, worker);
				Check(abort.method == "DELETE", "transient abort retries");
				Respond(abort, 204);
			}
		}
		Check(done && success == (scenario != 0), "upload completion outcome");
		Check(!client.Pending(), "upload state released");
	}

	// An endpoint that keeps answering 503 must exhaust the retry budget and abort the upload.
	// The re-read between attempts must not reset the counter: with the reset, the client retried
	// for ever and Pending() never cleared (which also blocked retention and config changes).
	{
		bool done = false;
		bool success = true;
		client.UploadFile(path.string(), "failing.zip", "application/zip", [&](k4::UploadOutcome result) {
			done = true;
			success = result.success;
		});
		auto create = Next(client, worker);
		Respond(create, 200, "<InitiateMultipartUploadResult><UploadId>test-id</UploadId></InitiateMultipartUploadResult>");
		int attempts = 0;
		bool aborted = false;
		for (int guard = 0; guard < 10 && !aborted; ++guard) {
			auto put = Next(client, worker);
			if (put.method == "DELETE") {
				aborted = true;
				Respond(put, 204);
				break;
			}
			Check(put.method == "PUT" && put.body.size() == size - 1, "persistent failure keeps the part body");
			Respond(put, 503);
			++attempts;
		}
		Check(attempts == 4, "persistent 503 exhausts the retry budget (initial attempt + 3 retries)");
		Check(aborted, "multipart upload is aborted after the retry budget");
		Check(done && !success, "persistent 503 fails the upload");
		Check(!client.Pending(), "failed upload releases the pending state");
	}
	worker.Stop(); http.Shutdown(); client.Shutdown();
	std::error_code error; std::filesystem::remove(path, error);
	std::cout << "S3 state-machine failures: " << failures << '\n';
	return failures;
}
