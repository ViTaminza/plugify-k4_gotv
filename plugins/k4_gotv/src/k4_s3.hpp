// k4_gotv — the S3 upload/retention client (AWS Signature V4, path-style, HTTPS only).
//
// The original uploaded the finished .zip to FTP/SFTP/Mega; this port uploads it to an
// S3-compatible endpoint instead. Files up to one part are PUT directly; larger files go through
// CreateMultipartUpload / UploadPart (parts are read from disk one at a time on the worker thread
// and never all in memory) / CompleteMultipartUpload, with AbortMultipartUpload on failure.
// Transient failures (network, 408/429/5xx) are retried with 1s/2s/4s back-off; 4xx errors are
// final. Uploads are only requested after tv_stoprecord finished writing the demo (the caller
// honours the original's 2 s delay).
//
// Retention: ListObjectsV2 under the prefix (paginated), then DeleteObject for every object older
// than RetentionDays. This replaces the original's uploads_retention.json bookkeeping - S3 can be
// listed, so no local record is needed.
//
// All methods run on the game thread; Pump() advances the state machines and is called from
// OnPluginUpdate.

#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "k4_core.hpp"

namespace k4 {

class HttpService;
class Worker;

struct UploadOutcome {
	bool success = false;
	std::string key;
	std::string url;
	std::string error; // bounded, never contains credentials
};

class S3Client {
public:
	S3Client();
	~S3Client();
	S3Client(const S3Client&) = delete;
	S3Client& operator=(const S3Client&) = delete;

	// `settings` is the parsed config. The client stays disabled (Configured() == false) unless
	// endpoint, bucket, access_key and secret_key are all present and the endpoint uses https.
	void Configure(const S3Settings& settings, HttpService* http, Worker* worker);
	bool Configured() const;
	const std::string& Prefix() const;
	const std::string& Bucket() const;

	// Key and public URL of an object name (file name including ".zip").
	std::string ObjectKey(std::string_view objectName) const;
	std::string ObjectUrl(std::string_view objectKey) const;

	// Queues one upload. `done` runs on the game thread once the object is complete or the upload
	// failed.
	void UploadFile(std::string filePath, std::string objectName, std::string contentType,
					std::function<void(UploadOutcome)> done);

	// Queues a retention pass; `done(deleted, error)` runs when it finishes (error empty on
	// success).
	void RetentionScan(std::time_t now, int retentionDays,
					   std::function<void(int, std::string)> done = {});

	// True while an upload or a retention pass is queued/running.
	bool Pending() const;

	void Pump();
	// Rewinds everything; no callbacks run (plugin unload / reload).
	void Shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace k4
