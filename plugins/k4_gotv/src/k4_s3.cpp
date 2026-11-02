// k4_gotv — the S3 upload/retention client (see k4_s3.hpp).
//
// Upload state machine:
//   file <= one part : read the file (one bounded buffer) -> PutObject
//   larger           : CreateMultipartUpload -> per part (worker read -> UploadPart) ->
//                      CompleteMultipartUpload; AbortMultipartUpload on an unrecoverable failure
// Transient failures (network/timeout, 408, 429, 5xx) are retried with 1s/2s/4s back-off; other
// HTTP errors are final. Parts are read from disk one at a time and never all held in memory.
//
// Everything here runs on the game thread. Worker completions arrive through Worker::Poll and
// HTTP responses through HttpService::Pump; both are pumped from OnPluginUpdate. Every callback
// checks the upload generation so a completion that outlives its upload (Shutdown) is ignored.

#include "k4_s3.hpp"

#include "k4_text.hpp"

#include "k4_crypto.hpp"
#include "k4_http.hpp"
#include "k4_s3sign.hpp"
#include "k4_s3util.hpp"
#include "k4_worker.hpp"

#include <plg/plugin.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <system_error>
#include <utility>

namespace k4 {
namespace {

constexpr const char* kLogTag = "k4_gotv:";
constexpr uint64_t kMiB = 1024ull * 1024ull;
// A single PUT holds the whole object in memory. Cap that at 64 MiB regardless of part_size_mb;
// larger files always use multipart.
constexpr uint64_t kMaximumSinglePut = 64ull * kMiB;
constexpr int kMaximumRetries = 3;
constexpr unsigned kMaximumListPages = 1000;
constexpr auto kUploadTimeout = std::chrono::seconds(300);
constexpr auto kControlTimeout = std::chrono::seconds(60);

void LogInfo(const std::string& message) {
	if (plg::Log) {
		plg::Log(std::string(kLogTag) + " " + k4::RedactSensitive(message), plg::Severity::Info,
				 plg::source_location::current());
	}
}

void LogWarning(const std::string& message) {
	if (plg::Log) {
		plg::Log(std::string(kLogTag) + " " + k4::RedactSensitive(message), plg::Severity::Warning,
				 plg::source_location::current());
	}
}

void LogError(const std::string& message) {
	if (plg::Log) {
		plg::Log(std::string(kLogTag) + " " + k4::RedactSensitive(message), plg::Severity::Error,
				 plg::source_location::current());
	}
}

bool IsTransientStatus(int status) {
	return status == 0 || status == 408 || status == 429 || (status >= 500 && status <= 599);
}

std::chrono::steady_clock::duration Backoff(int attempts) {
	switch (attempts) {
	case 1: return std::chrono::seconds(1);
	case 2: return std::chrono::seconds(2);
	case 3: return std::chrono::seconds(4);
	default: return std::chrono::seconds(8);
	}
}

std::string HttpErrorText(const HttpResponse& response) {
	if (response.transportFailed) {
		// Distinguish "no HTTP response at all" from an HTTP error: the transport name and the
		// Steam/curl failure detail make the next live log self-explanatory.
		std::string text = response.transport.empty() ? "transport failure" : response.transport + " transport failure";
		if (!response.transportError.empty()) {
			text += " (" + response.transportError + ")";
		}
		return text;
	}
	std::string text = "HTTP " + std::to_string(response.status);
	const S3ErrorInfo error = ParseS3Error(response.body);
	std::string detail = error.code;
	if (!error.message.empty()) {
		if (!detail.empty()) {
			detail += ": ";
		}
		detail += error.message;
	}
	if (detail.empty() && !response.body.empty()) {
		detail = response.body.substr(0, 200);
	}
	// Keep the log line printable and bounded; S3 error bodies never contain the secret key.
	std::string printable;
	for (const unsigned char character : detail) {
		printable.push_back(character >= 0x20 && character < 0x7f ? static_cast<char>(character) : ' ');
	}
	constexpr std::size_t kMaximumErrorLength = 300;
	if (printable.size() > kMaximumErrorLength) {
		printable.resize(kMaximumErrorLength);
		printable += "...";
	}
	if (!printable.empty()) {
		text += " (" + printable + ")";
	}
	// Defensive: an error body that quotes a request must not leak a query credential.
	return k4::RedactSensitive(text);
}

bool ReadFileRange(const std::string& path, uint64_t offset, uint64_t length, std::string* out) {
	std::ifstream input(path, std::ios::binary);
	if (!input) {
		return false;
	}
	input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
	if (!input) {
		return false;
	}
	if (length > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())) {
		return false;
	}
	out->resize(static_cast<std::size_t>(length));
	if (length == 0) {
		return true;
	}
	input.read(out->data(), static_cast<std::streamsize>(length));
	return static_cast<uint64_t>(input.gcount()) == length;
}

} // namespace

struct S3Client::Impl {
	struct Upload {
		uint64_t generation = 0;
		std::string path;
		std::string objectName;
		std::string key;
		std::string url;
		std::string contentType;
		uint64_t fileSize = 0;
		UploadPlan plan;
		bool multipart = false;
		std::string uploadId;
		std::string verificationId;
		std::vector<std::pair<int, std::string>> etags;
		std::size_t nextPart = 0;

		enum class Step { ReadWhole, PutWhole, Create, ReadPart, PutPart, Complete, Verify, Abort, Finish }
			step = Step::ReadWhole;
		int attempts = 0;
		std::chrono::steady_clock::time_point retryAt {};
		bool waiting = false;
		std::string partData; // whole-file body or the current part; moved into the request
		std::string partHash; // its SHA-256, computed on the file worker (never on the game thread)
		std::size_t partIndex = 0;
		std::function<void(UploadOutcome)> done;
		std::string error;
	};

	struct Retention {
		bool active = false;
		bool waiting = false;
		std::time_t now = 0;
		int days = 3;
		std::string token;
		bool firstPage = true;
		unsigned pages = 0;
		std::vector<ObjectInfo> objects;
		std::vector<std::string> toDelete;
		std::size_t nextDelete = 0;
		int deleted = 0;
		int attempts = 0;
		std::chrono::steady_clock::time_point retryAt {};
		enum class Step { List, Delete, Finish } step = Step::List;
		std::function<void(int, std::string)> done;
		std::string error;
	};

	S3Settings settings;
	std::string host;
	HttpService* http = nullptr;
	Worker* worker = nullptr;
	bool configured = false;
	uint64_t generation = 0;

	std::deque<Upload> queue;
	std::unique_ptr<Upload> upload;
	Retention retention;
	bool retentionQueued = false;
	std::time_t retentionQueuedNow = 0;
	int retentionQueuedDays = 3;
	std::function<void(int, std::string)> retentionQueuedDone;

	Upload* Current(uint64_t generationValue) {
		if (!upload || upload->generation != generationValue) {
			return nullptr;
		}
		return upload.get();
	}

	void FinishUpload(bool success, std::string error);
	void StartNextUpload();
	void PumpUpload();
	void DispatchUpload(Upload& up);

	void StartRetention();
	void PumpRetention();
	void DispatchRetention();
	void FinishRetention(std::string error);

	HttpRequest BuildRequest(const std::string& method, const std::string& key, bool bucketLevel,
							 std::vector<std::pair<std::string, std::string>> query, std::string body,
							 const std::string& contentType, std::vector<std::string> captureHeaders,
							 std::chrono::milliseconds timeout, std::function<void(HttpResponse)> onDone,
							 std::vector<HttpHeader> extraHeaders = {}, const std::string& payloadHash = {});

	void RetryOrFail(Upload& up, int status, const std::string& error, Upload::Step nextOnFailure);
	void ScheduleRetry(Upload& up);
	void BeginAbort(Upload& up);
};

void S3Client::Impl::ScheduleRetry(Upload& up) {
	++up.attempts;
	up.retryAt = std::chrono::steady_clock::now() + Backoff(up.attempts);
}

void S3Client::Impl::BeginAbort(Upload& up) {
	if (!up.multipart || up.uploadId.empty()) {
		FinishUpload(false, up.error);
		return;
	}
	up.step = Upload::Step::Abort;
	up.attempts = 0;
	up.retryAt = {};
	up.waiting = false;
}

void S3Client::Impl::FinishUpload(bool success, std::string error) {
	UploadOutcome outcome;
	if (upload) {
		outcome.key = upload->key;
		outcome.url = upload->url;
	}
	outcome.success = success;
	outcome.error = std::move(error);
	std::function<void(UploadOutcome)> done;
	if (upload) {
		done = std::move(upload->done);
	}
	upload.reset();
	if (done) {
		done(std::move(outcome));
	}
}

void S3Client::Impl::StartNextUpload() {
	// A queued retention pass never blocks uploads: the pass starts only once the upload queue is
	// empty (PumpRetention), otherwise both sides would wait for each other.
	if (upload || queue.empty() || retention.active) {
		return;
	}
	upload = std::make_unique<Upload>(std::move(queue.front()));
	queue.pop_front();
	upload->generation = ++generation;
	upload->verificationId = Sha256Hex(upload->path + ":" + std::to_string(
		std::chrono::system_clock::now().time_since_epoch().count()) + ":" + std::to_string(generation));
	upload->attempts = 0;
	upload->retryAt = {};

	std::error_code filesystemError;
	const uint64_t fileSize = std::filesystem::file_size(upload->path, filesystemError);
	if (filesystemError) {
		FinishUpload(false, "cannot stat " + upload->path + ": " + filesystemError.message());
		return;
	}
	upload->fileSize = fileSize;

	const uint64_t requestedPart = static_cast<uint64_t>(settings.partSizeMb) * kMiB;
	const uint64_t effectivePart = std::max<uint64_t>(5 * kMiB, std::min(requestedPart, kMaximumSinglePut));
	upload->plan = PlanUpload(fileSize, effectivePart);
	upload->multipart = upload->plan.useMultipart;
	upload->step = upload->multipart ? Upload::Step::Create : Upload::Step::ReadWhole;
	LogInfo("S3 upload starting: " + upload->key + " (" + std::to_string(fileSize) + " bytes, "
			+ (upload->multipart ? std::to_string(upload->plan.parts.size()) + " parts" : "single PUT") + ")");
}

void S3Client::Impl::RetryOrFail(Upload& up, int status, const std::string& error, Upload::Step nextOnFailure) {
	if (IsTransientStatus(status) && up.attempts < kMaximumRetries) {
		ScheduleRetry(up);
		return;
	}
	up.error = error;
	if (up.multipart && !up.uploadId.empty() && nextOnFailure != Upload::Step::Abort) {
		BeginAbort(up);
	} else {
		FinishUpload(false, error);
	}
}

void S3Client::Impl::PumpUpload() {
	if (!upload && !queue.empty() && !retention.active) {
		StartNextUpload();
	}
	if (!upload) {
		return;
	}
	Upload& up = *upload;
	if (up.waiting) {
		return;
	}
	if (std::chrono::steady_clock::now() < up.retryAt) {
		return;
	}
	DispatchUpload(up);
}

void S3Client::Impl::DispatchUpload(Upload& up) {
	const uint64_t generationValue = up.generation;
	Impl* self = this;

	switch (up.step) {
	case Upload::Step::ReadWhole: {
		up.waiting = true;
		const std::string path = up.path;
		const uint64_t size = up.fileSize;
		if (worker == nullptr) {
			up.waiting = false;
			FinishUpload(false, "file worker unavailable");
			return;
		}
		worker->Post([self, generationValue, path, size]() -> std::function<void()> {
			std::string data;
			const bool ok = size <= kMaximumSinglePut && ReadFileRange(path, 0, size, &data);
			// Hash on the worker as well: the body can be up to 64 MiB and this runs inside
			// OnPluginUpdate on the game thread otherwise.
			std::string hash = ok ? Sha256Hex(data) : std::string();
			return [self, generationValue, ok, hash = std::move(hash), data = std::move(data)]() mutable {
				Upload* current = self->Current(generationValue);
				if (current == nullptr) {
					return;
				}
				current->waiting = false;
				if (!ok) {
					current->error = "cannot read " + current->path;
					self->FinishUpload(false, current->error);
					return;
				}
				current->partData = std::move(data);
				current->partHash = std::move(hash);
				// No attempts reset here: a re-read on retry must keep the retry budget
				// (StartNextUpload already starts every upload at 0).
				current->step = Upload::Step::PutWhole;
			};
		});
		break;
	}
	case Upload::Step::PutWhole: {
		up.waiting = true;
		// Move the body into the request instead of copying it on the game thread; a retry goes
		// back to ReadWhole and lets the worker read and hash the file again.
		std::string body = std::move(up.partData);
		up.partData.clear();
		http->Enqueue(BuildRequest("PUT", up.key, false, {}, std::move(body), up.contentType, {},
								   kUploadTimeout, [self, generationValue](HttpResponse response) {
									   Upload* current = self->Current(generationValue);
									   if (current == nullptr) {
										   return;
									   }
									   current->waiting = false;
									   if (!response.transportFailed && response.status >= 200 && response.status <= 299) {
										   self->FinishUpload(true, {});
										   return;
									   }
									   current->step = Upload::Step::ReadWhole;
									   self->RetryOrFail(*current, response.transportFailed ? 0 : response.status,
														 HttpErrorText(response), Upload::Step::PutWhole);
								   },
								   {}, up.partHash));
		break;
	}
	case Upload::Step::Create: {
		up.waiting = true;
		http->Enqueue(BuildRequest("POST", up.key, false, {{"uploads", ""}}, {}, up.contentType, {},
								   kControlTimeout, [self, generationValue](HttpResponse response) {
									   Upload* current = self->Current(generationValue);
									   if (current == nullptr) {
										   return;
									   }
									   current->waiting = false;
									   if (response.transportFailed || response.status < 200 || response.status > 299) {
										   self->RetryOrFail(*current, response.transportFailed ? 0 : response.status,
															 HttpErrorText(response), Upload::Step::Create);
										   return;
									   }
									   const std::string uploadId = ParseUploadId(response.body);
									   if (uploadId.empty()) {
									   self->RetryOrFail(*current, 500, "CreateMultipartUpload returned no UploadId",
														 Upload::Step::Create);
										   return;
									   }
									   current->uploadId = uploadId;
									   current->attempts = 0;
									   current->nextPart = 0;
									   current->step = Upload::Step::ReadPart;
								   }));
		break;
	}
	case Upload::Step::ReadPart: {
		if (up.nextPart >= up.plan.parts.size()) {
			up.attempts = 0;
			up.step = Upload::Step::Complete;
			return;
		}
		if (worker == nullptr) {
			up.error = "file worker unavailable";
			BeginAbort(up);
			return;
		}
		const PartPlan part = up.plan.parts[up.nextPart];
		const std::size_t index = up.nextPart;
		const std::string path = up.path;
		up.waiting = true;
		up.partIndex = index;
		worker->Post([self, generationValue, path, part, index]() -> std::function<void()> {
			std::string data;
			const bool ok = ReadFileRange(path, part.offset, part.length, &data);
			// The part is up to 64 MiB: hash it here, on the worker, not in OnPluginUpdate.
			std::string hash = ok ? Sha256Hex(data) : std::string();
			return [self, generationValue, ok, index, hash = std::move(hash), data = std::move(data)]() mutable {
				Upload* current = self->Current(generationValue);
				if (current == nullptr) {
					return;
				}
				current->waiting = false;
				if (index != current->partIndex) {
					return;
				}
				if (!ok) {
					current->error = "cannot read part " + std::to_string(index + 1) + " from " + current->path;
					self->BeginAbort(*current);
					return;
				}
				current->partData = std::move(data);
				current->partHash = std::move(hash);
				// A retry re-reads the same part; the attempts counter must survive that (it is
				// reset by the successful part PUT below, or by StartNextUpload for a new upload).
				current->step = Upload::Step::PutPart;
			};
		});
		break;
	}
	case Upload::Step::PutPart: {
		const int partNumber = static_cast<int>(up.partIndex) + 1;
		up.waiting = true;
		// Moved into the request; a retry goes back to ReadPart for the same part.
		std::string body = std::move(up.partData);
		up.partData.clear();
		http->Enqueue(BuildRequest(
			"PUT", up.key, false,
			{{"partNumber", std::to_string(partNumber)}, {"uploadId", up.uploadId}}, std::move(body),
			"application/octet-stream", {"ETag"}, kUploadTimeout,
			[self, generationValue, partNumber](HttpResponse response) {
				Upload* current = self->Current(generationValue);
				if (current == nullptr) {
					return;
				}
				current->waiting = false;
				if (response.transportFailed || response.status < 200 || response.status > 299) {
					current->step = Upload::Step::ReadPart;
					self->RetryOrFail(*current, response.transportFailed ? 0 : response.status,
									  HttpErrorText(response), Upload::Step::PutPart);
					return;
				}
				std::string etag;
				for (const auto& [name, value] : response.headers) {
					if (name == "ETag" || name == "etag" || name == "Etag") {
						etag = value;
						break;
					}
				}
				if (etag.empty()) {
					current->error = "UploadPart " + std::to_string(partNumber) + " returned no ETag";
					self->BeginAbort(*current);
					return;
				}
				current->etags.emplace_back(partNumber, std::move(etag));
				++current->nextPart;
				current->attempts = 0;
				current->step = Upload::Step::ReadPart;
			},
			{}, up.partHash));
		break;
	}
	case Upload::Step::Complete: {
		up.waiting = true;
		http->Enqueue(BuildRequest("POST", up.key, false, {{"uploadId", up.uploadId}},
								   BuildCompleteMultipartXml(up.etags), "application/xml", {}, kControlTimeout,
								   [self, generationValue](HttpResponse response) {
									   Upload* current = self->Current(generationValue);
									   if (current == nullptr) {
										   return;
									   }
									   current->waiting = false;
									   if (response.transportFailed || response.status < 200 || response.status > 299
										   || IsS3ErrorDocument(response.body)) {
										   if (response.status == 200 && IsS3ErrorDocument(response.body)) {
											   const std::string code = ParseS3Error(response.body).code;
											   if (code == "InternalError" || code == "SlowDown" || code == "ServiceUnavailable") {
												   response.status = 500;
											   }
										   }
										   if (!response.transportFailed && response.status == 404) {
											   // The upload id is gone: either the Complete was committed and
											   // its response lost, or the upload expired/was aborted. Ask
											   // S3 whether the object exists before declaring a failure.
											   current->attempts = 0;
											   current->step = Upload::Step::Verify;
											   return;
										   }
										   self->RetryOrFail(*current, response.transportFailed ? 0 : response.status,
															 HttpErrorText(response), Upload::Step::Complete);
										   return;
									   }
									   self->FinishUpload(true, {});
								   }));
		break;
	}
	case Upload::Step::Verify: {
		// HEAD the object after NoSuchUpload on Complete (lost response ambiguity). The random
		// x-amz-meta-k4-upload-id stored by CreateMultipartUpload identifies *this* upload; the
		// Content-Length is compared too when the HTTP layer exposes it (a missing standard header
		// must not turn a committed upload into an abort).
		up.waiting = true;
		http->Enqueue(BuildRequest("HEAD", up.key, false, {}, {}, {},
								   {"x-amz-meta-k4-upload-id", "Content-Length"}, kControlTimeout,
								   [self, generationValue](HttpResponse response) {
									   Upload* current = self->Current(generationValue);
									   if (current == nullptr) {
										   return;
									   }
									   current->waiting = false;
									   if (!response.transportFailed && response.status >= 200 && response.status <= 299) {
										   bool identityMatches = false;
										   bool sawSize = false;
										   bool sizeMatches = false;
										   for (const auto& [name, value] : response.headers) {
											   if (name == "x-amz-meta-k4-upload-id") {
												   identityMatches = value == current->verificationId;
											   } else if (name == "Content-Length") {
												   sawSize = true;
												   sizeMatches = value == std::to_string(current->fileSize);
											   }
										   }
										   if (!identityMatches || (sawSize && !sizeMatches)) {
											   current->error = "CompleteMultipartUpload: HEAD did not identify this upload";
											   self->BeginAbort(*current);
											   return;
										   }
										   LogInfo("S3 upload verified after a lost Complete response: " + current->key);
										   self->FinishUpload(true, {});
										   return;
									   }
									   if ((response.transportFailed || IsTransientStatus(response.status))
										   && current->attempts < kMaximumRetries) {
										   ++current->attempts;
										   current->retryAt = std::chrono::steady_clock::now() + Backoff(current->attempts);
										   return;
									   }
									   current->error = "CompleteMultipartUpload: " + HttpErrorText(response);
									   self->BeginAbort(*current);
								   }));
		break;
	}
	case Upload::Step::Abort: {
		up.waiting = true;
		http->Enqueue(BuildRequest("DELETE", up.key, false, {{"uploadId", up.uploadId}}, {}, {}, {},
								   kControlTimeout, [self, generationValue](HttpResponse response) {
									   Upload* current = self->Current(generationValue);
									   if (current == nullptr) {
										   return;
									   }
									   current->waiting = false;
									   if (response.transportFailed || response.status < 200 || response.status > 299) {
										   const int status = response.transportFailed ? 0 : response.status;
											   if (IsTransientStatus(status) && current->attempts < kMaximumRetries) {
												   self->ScheduleRetry(*current);
												   return;
											   }
											   LogWarning("S3 AbortMultipartUpload failed: " + HttpErrorText(response));
									   }
									   self->FinishUpload(false, current->error);
								   }));
		break;
	}
	case Upload::Step::Finish:
		FinishUpload(false, up.error);
		break;
	}
}

void S3Client::Impl::StartRetention() {
	if (retention.active || !retentionQueued || upload || !queue.empty()) {
		return;
	}
	retention = Retention {};
	retention.active = true;
	retention.now = retentionQueuedNow;
	retention.days = retentionQueuedDays;
	retention.done = std::move(retentionQueuedDone);
	retentionQueued = false;
	retentionQueuedDone = nullptr;
	retention.step = Retention::Step::List;
}

void S3Client::Impl::FinishRetention(std::string error) {
	std::function<void(int, std::string)> done = std::move(retention.done);
	const int deleted = retention.deleted;
	retention = Retention {};
	retention.active = false;
	if (done) {
		done(deleted, std::move(error));
	}
}

void S3Client::Impl::DispatchRetention() {
	Impl* self = this;
	Retention& state = retention;
	switch (state.step) {
	case Retention::Step::List: {
		std::vector<std::pair<std::string, std::string>> query = {
			{"list-type", "2"},
			{"max-keys", "1000"},
			{"prefix", settings.prefix},
		};
		if (!state.token.empty()) {
			query.emplace_back("continuation-token", state.token);
		}
		state.waiting = true;
		http->Enqueue(BuildRequest("GET", "", true, std::move(query), {}, {}, {}, kControlTimeout,
								   [self](HttpResponse response) {
									   if (!self->retention.active) {
										   return;
									   }
									   Retention& current = self->retention;
									   current.waiting = false;
									   if (response.transportFailed || response.status < 200 || response.status > 299) {
										   if (IsTransientStatus(response.transportFailed ? 0 : response.status)
											   && current.attempts < kMaximumRetries) {
											   ++current.attempts;
											   current.retryAt = std::chrono::steady_clock::now() + Backoff(current.attempts);
											   return;
										   }
										   self->FinishRetention(HttpErrorText(response));
										   return;
									   }
									   const ListObjectsResult listed = ParseListObjectsV2(response.body);
									   if (!listed.errorCode.empty()) {
										   self->FinishRetention("ListObjectsV2: " + listed.errorCode
																 + (listed.errorMessage.empty() ? "" : ": " + listed.errorMessage));
										   return;
									   }
									   current.objects.insert(current.objects.end(), listed.objects.begin(), listed.objects.end());
									   current.attempts = 0;
									   ++current.pages;
									   if (listed.truncated && !listed.nextContinuationToken.empty()
										   && current.pages < kMaximumListPages) {
										   current.token = listed.nextContinuationToken;
										   current.firstPage = false;
										   return;
									   }
									   current.toDelete = SelectExpiredObjects(current.objects, current.now, current.days);
									   current.nextDelete = 0;
									   current.step = Retention::Step::Delete;
								   }));
		break;
	}
	case Retention::Step::Delete: {
		if (state.nextDelete >= state.toDelete.size()) {
			FinishRetention(state.error); // reports the first per-object failure, if any
			return;
		}
		const std::string key = state.toDelete[state.nextDelete];
		state.waiting = true;
		http->Enqueue(BuildRequest("DELETE", key, false, {}, {}, {}, {}, kControlTimeout,
								   [self, key](HttpResponse response) {
									   if (!self->retention.active) {
										   return;
									   }
									   Retention& current = self->retention;
									   current.waiting = false;
									   if (!response.transportFailed
										   && ((response.status >= 200 && response.status <= 299) || response.status == 404)) {
										   ++current.deleted;
										   ++current.nextDelete;
										   current.attempts = 0;
										   return;
									   }
									   if (IsTransientStatus(response.transportFailed ? 0 : response.status)
										   && current.attempts < kMaximumRetries) {
										   ++current.attempts;
										   current.retryAt = std::chrono::steady_clock::now() + Backoff(current.attempts);
										   return;
									   }
									   // One undeletable object (e.g. 403) must not leave every other expired
									   // object behind: remember the first error and continue the pass.
									   if (current.error.empty()) {
										   current.error = "DeleteObject " + key + ": " + HttpErrorText(response);
									   }
									   ++current.nextDelete;
									   current.attempts = 0;
								   }));
		break;
	}
	case Retention::Step::Finish:
		FinishRetention(state.error);
		break;
	}
}

void S3Client::Impl::PumpRetention() {
	if (!retention.active) {
		if (retentionQueued && !upload && queue.empty()) {
			StartRetention();
		}
		if (!retention.active) {
			return;
		}
	}
	if (retention.waiting || std::chrono::steady_clock::now() < retention.retryAt) {
		return;
	}
	DispatchRetention();
}

HttpRequest S3Client::Impl::BuildRequest(const std::string& method, const std::string& key, bool bucketLevel,
										 std::vector<std::pair<std::string, std::string>> query, std::string body,
										 const std::string& contentType, std::vector<std::string> captureHeaders,
										 std::chrono::milliseconds timeout,
										 std::function<void(HttpResponse)> onDone,
										 std::vector<HttpHeader> extraHeaders, const std::string& payloadHash) {
	std::vector<std::pair<std::string, std::string>> extra;
	if (upload && method == "POST"
		&& std::any_of(query.begin(), query.end(), [](const auto& item) { return item.first == "uploads"; })) {
		extra.emplace_back("x-amz-meta-k4-upload-id", upload->verificationId);
	}
	for (const HttpHeader& header : extraHeaders) {
		extra.emplace_back(header.name, header.value);
	}
	// Same builder as the host-side probe: identical URL, signed headers and payload hash. Large
	// bodies arrive with their hash (computed on the file worker); only small control requests are
	// hashed here.
	S3SignedRequest signedRequest =
		payloadHash.empty()
			? BuildS3SignedRequest(settings, host, method, key, bucketLevel, query, std::move(body), contentType, extra)
			: BuildS3SignedRequest(settings, host, method, key, bucketLevel, query, std::move(body), contentType,
								   payloadHash, extra);

	HttpRequest request;
	request.method = method;
	request.url = std::move(signedRequest.url);
	for (auto& [name, value] : signedRequest.headers) {
		request.headers.push_back({std::move(name), std::move(value)});
	}
	request.contentType = std::move(signedRequest.contentType);
	request.body = std::move(signedRequest.body);
	request.captureHeaders = std::move(captureHeaders);
	request.timeout = timeout;
	request.onDone = std::move(onDone);
	return request;
}

S3Client::S3Client() : m_impl(std::make_unique<Impl>()) {}

S3Client::~S3Client() {
	Shutdown();
}

void S3Client::Configure(const S3Settings& settings, HttpService* http, Worker* worker) {
	Impl& impl = *m_impl;
	impl.settings = settings;
	impl.http = http;
	impl.worker = worker;
	impl.host = NormalizeEndpointHost(settings.endpoint);
	impl.configured = !settings.endpoint.empty() && !settings.bucket.empty() && !settings.accessKey.empty()
					  && !settings.secretKey.empty() && http != nullptr && worker != nullptr;
	if (!impl.configured) {
		// A partially configured S3 block is a configuration error worth a line, but the plugin
		// keeps working without uploads (like the original with every upload service disabled).
		if (!settings.endpoint.empty() || !settings.bucket.empty() || !settings.accessKey.empty()
			|| !settings.secretKey.empty()) {
			LogWarning("S3 is not configured completely (endpoint, bucket, access_key and secret_key are all "
					   "required); uploads are disabled.");
		}
		return;
	}
	if (!EndpointIsHttps(settings.endpoint) || impl.host.empty() || settings.bucket.find('/') != std::string::npos) {
		LogError("S3 is disabled: the endpoint must be an HTTPS host without a path.");
		impl.configured = false;
		return;
	}
	if (impl.settings.region.empty()) {
		impl.settings.region = "us-east-1";
	}
	if (impl.settings.partSizeMb < 5) {
		impl.settings.partSizeMb = 5;
	}
	LogInfo("S3 uploads enabled: https://" + impl.host + "/" + settings.bucket + "/" + settings.prefix
			+ " (region " + impl.settings.region + ")");
}

bool S3Client::Configured() const {
	return m_impl->configured;
}

const std::string& S3Client::Prefix() const {
	return m_impl->settings.prefix;
}

const std::string& S3Client::Bucket() const {
	return m_impl->settings.bucket;
}

std::string S3Client::ObjectKey(std::string_view objectName) const {
	return S3ObjectKey(m_impl->settings.prefix, objectName);
}

std::string S3Client::ObjectUrl(std::string_view objectKey) const {
	return S3ObjectUrl(m_impl->host, m_impl->settings.bucket, objectKey, m_impl->settings.pathStyle);
}

void S3Client::UploadFile(std::string filePath, std::string objectName, std::string contentType,
						  std::function<void(UploadOutcome)> done) {
	Impl::Upload upload;
	upload.path = std::move(filePath);
	upload.objectName = std::move(objectName);
	upload.key = ObjectKey(upload.objectName);
	upload.url = ObjectUrl(upload.key);
	upload.contentType = std::move(contentType);
	upload.done = std::move(done);
	m_impl->queue.push_back(std::move(upload));
}

void S3Client::RetentionScan(std::time_t now, int retentionDays, std::function<void(int, std::string)> done) {
	Impl& impl = *m_impl;
	if (impl.retention.active || impl.retentionQueued) {
		LogInfo("S3 retention pass skipped: the previous pass is still running.");
		return;
	}
	if (!impl.configured) {
		if (done) {
			done(0, "S3 is not configured");
		}
		return;
	}
	impl.retentionQueued = true;
	impl.retentionQueuedNow = now;
	impl.retentionQueuedDays = retentionDays;
	impl.retentionQueuedDone = std::move(done);
}

bool S3Client::Pending() const {
	return m_impl->upload != nullptr || !m_impl->queue.empty() || m_impl->retention.active || m_impl->retentionQueued;
}

void S3Client::Pump() {
	if (!m_impl->configured) {
		return;
	}
	m_impl->PumpUpload();
	m_impl->PumpRetention();
}

void S3Client::Shutdown() {
	Impl& impl = *m_impl;
	impl.queue.clear();
	impl.upload.reset();
	impl.retentionQueued = false;
	impl.retentionQueuedDone = nullptr;
	impl.retention = Impl::Retention {};
}

} // namespace k4
