// k4_gotv — engine-free core: configuration, file naming, time formatting, upload planning,
// retention selection, S3 key/URL construction.
//
// Everything here is pure logic (no s2sdk, no Plugify, no threads) so tests/k4_gotv_tests.cpp can
// drive it directly.

#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace k4 {

// --- configuration (k4-gotv.jsonc of the original, section K4GOTV) ---------------------------

struct GeneralSettings {
	double minimumDemoDuration = 5.0;
	bool deleteDemoAfterUpload = true;
	bool deleteZippedDemoAfterUpload = true;
	bool deleteEveryDemoFromServerAfterServerStart = false;
	bool logUploads = true;
	bool logDeletions = true;
	std::string regularFileNamingPattern = "{fileName}_{map}_{date}_{time}";
	std::string cropRoundsFileNamingPattern = "{fileName}_{map}_round{round}_{date}_{time}";
	std::string demoDirectory = "demos";
	bool autoCleanupEnabled = false;
	int autoCleanupIntervalMinutes = 60;
	int autoCleanupFileAgeHours = 48;
};

struct DiscordSettings {
	std::string webhookUrl;
	std::string webhookAvatar;
	bool webhookUploadFile = true;
	std::string webhookName = "CSGO Demo Bot";
	std::string embedTitle = "New CSGO Demo Available";
	std::string messageText = "@everyone New CSGO Demo Available!";
	int serverBoost = 0;

	// Discord's per-file upload limit in MB for the configured boost level (0 -> 25, 2 -> 50,
	// 3 -> 100; any other value falls back to 25, like the original's switch).
	int maxDiscordFileSizeMb() const;
};

struct AutoRecordSettings {
	bool enabled = false;
	bool cropRounds = false;
	bool stopOnIdle = false;
	bool recordWarmup = true;
	int idlePlayerCountThreshold = 0;
	int idleTimeSeconds = 300;
};

struct DemoRequestSettings {
	bool enabled = false;
	bool printAll = true;
	bool deleteUnused = true;
};

// S3 replaces the original's FTP/SFTP/Mega block. The original had no S3 support at all; the
// fields are the ones the task specifies, plus the retention settings the original had for FTP
// and Mega (RetentionEnabled/RetentionHours, here in days as requested).
struct S3Settings {
	std::string endpoint;			// host or https://host, no path (port allowed)
	std::string region = "us-east-1";
	std::string bucket;
	std::string accessKey;
	std::string secretKey;
	std::string prefix = "demos/<server>/";
	bool pathStyle = true;
	int partSizeMb = 16;
	bool retentionEnabled = false;
	int retentionDays = 3;
};

// HTTP transport for both the S3 uploader and the Discord webhook. "auto" (default) uses the
// dlopen'ed libcurl whenever it loads, because the live server's ISteamHTTP delivers the PUT and
// then loses the response; ISteamHTTP remains as the fallback and as the explicit "steam" mode.
struct HttpSettings {
	std::string transport = "auto"; // auto | steam | curl
	std::string caFile;				// optional CA bundle path for the curl transport
};

struct Config {
	GeneralSettings general;
	DiscordSettings discord;
	AutoRecordSettings autoRecord;
	DemoRequestSettings demoRequest;
	S3Settings s3;
	HttpSettings http;

	// The original had these as separate services; kept as one switch because FTP/SFTP/Mega are
	// not ported.
	bool s3Enabled() const { return !s3.endpoint.empty() && !s3.bucket.empty() && !s3.accessKey.empty() && !s3.secretKey.empty(); }
	int maxDiscordFileSizeMb() const { return discord.maxDiscordFileSizeMb(); }
};

Config DefaultConfig();
// Parses the text of config.jsonc. Returns false only when the JSON is unreadable (out is then
// DefaultConfig()). A root "K4GOTV" object (the original's section) is accepted; without it the
// root itself is the settings object. Individual wrong-typed values keep their default and append
// a message to `warning`. `ignore_comments` (JSONC) is always on. NormalizeConfig() must be called
// afterwards (the plugin does; the Clamping and the DemoRequest rule live there).
bool ParseConfig(std::string_view text, Config& out, std::string& warning);
// Post-processing: clamp part_size_mb, retention_days, intervals, and apply the original's
// DemoRequest rule (Enabled forces AutoRecord.Enabled and AutoRecord.CropRounds on).
void NormalizeConfig(Config& config);

// --- time / formatting -----------------------------------------------------------------------

std::string LocalDate(std::time_t when);		// yyyy-MM-dd
std::string LocalClock(std::time_t when);		// HH:mm:ss
std::string LocalFileTime(std::time_t when);	// HH-mm-ss
std::string LocalTimestamp(std::time_t when);	// yyyyMMdd_HHmmss
std::string LocalDateTime(std::time_t when);	// yyyy-MM-dd HH:mm:ss
std::string UtcIso8601(std::time_t when);		// 2026-10-18T13:54:09.000Z
// Duration in whole minutes:seconds ("05:07"), like C#'s TimeSpan.ToString(@"mm\:ss"): the
// minutes component is 0-59, so an hour-long demo prints "00:xx" exactly like the original.
std::string FormatDuration(double seconds);
// ISO-8601 timestamp (S3 LastModified, e.g. "2013-05-24T00:00:00.000Z"); 0 when it cannot be
// parsed.
std::time_t ParseIso8601(std::string_view text);

// --- demo file naming ------------------------------------------------------------------------

struct NamingContext {
	std::string fileName;	 // base name ("autodemo")
	std::string map;		 // s2sdk GetCurrentMap()
	std::string date;		 // yyyy-MM-dd
	std::string time;		 // HH-mm-ss
	std::string timestamp;	 // yyyyMMdd_HHmmss
	int round = 1;
	int playerCount = 0;
};

// Replaces {fileName} {map} {date} {time} {timestamp} {round} {playerCount}; unknown placeholders
// stay untouched (same as the original's string.Replace chain).
std::string BuildDemoFileName(std::string_view pattern, const NamingContext& context);
// name + "_<suffix>" (the original appends _1, _2, ... while the file exists).
std::string AppendCollisionSuffix(std::string_view base, int suffix);

// --- upload planning -------------------------------------------------------------------------

struct PartPlan {
	uint64_t offset = 0;
	uint64_t length = 0;
};

struct UploadPlan {
	bool useMultipart = false;
	uint64_t partSize = 0;
	std::vector<PartPlan> parts;
};

// Files up to one part use a single PUT; larger files are split into `partSize` chunks. S3 allows
// at most 10000 parts, so the part size is raised (to a whole MiB) when that would be exceeded;
// it never drops below 5 MiB (the S3 minimum for non-final parts).
UploadPlan PlanUpload(uint64_t fileSize, uint64_t requestedPartSize, uint32_t maxParts = 10000);

// --- retention -------------------------------------------------------------------------------

struct ObjectInfo {
	std::string key;
	std::time_t lastModified = 0;
};

// Keys of the objects at least `retentionDays` days old at `now` (the original deleted records
// whose age was >= RetentionHours). Objects without a parseable timestamp are kept.
std::vector<std::string> SelectExpiredObjects(const std::vector<ObjectInfo>& objects, std::time_t now,
											  int retentionDays);

// --- S3 keys / URLs --------------------------------------------------------------------------

// Adds a trailing '/' when non-empty and strips leading slashes; "<server>" is kept literally
// (the operator replaces it, the shipped default is "demos/<server>/").
std::string NormalizeS3Prefix(std::string_view prefix);
// prefix + fileName; the caller passes the stored file name including ".zip".
std::string S3ObjectKey(std::string_view prefix, std::string_view fileName);
// Strips a leading https:// or http:// and trailing slashes. Returns "" for a value with a path
// component or any whitespace. A scheme other than https is rejected by the caller (the
// credentials must not travel in plaintext).
std::string NormalizeEndpointHost(std::string_view endpoint);
bool EndpointIsHttps(std::string_view endpoint); // true when no scheme or https://
// Path-style: https://host/bucket/<percent-encoded key>. Virtual-hosted:
// https://bucket.host/<percent-encoded key>.
std::string S3ObjectUrl(std::string_view endpointHost, std::string_view bucket, std::string_view key,
						bool pathStyle);

} // namespace k4
