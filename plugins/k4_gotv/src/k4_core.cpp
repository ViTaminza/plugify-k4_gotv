// k4_gotv — engine-free core (see k4_core.hpp): config.jsonc parsing, time formats, demo naming,
// upload planning, retention selection, S3 keys/URLs.

#include "k4_core.hpp"

#include "k4_sigv4.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace k4 {
namespace {

constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr uint64_t kMinimumPartSize = 5 * kMiB;
constexpr uint64_t kMaximumPartSize = 5120ull * kMiB;

std::string Lowercase(std::string value) {
	for (char& character : value) {
		character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	}
	return value;
}

// Credentials and host names pasted into a JSONC file sometimes carry a stray space; a trailing
// space in the secret key would make every signature fail with no hint at all.
std::string TrimWhitespace(std::string value) {
	std::size_t begin = 0;
	while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
		++begin;
	}
	std::size_t end = value.size();
	while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
		--end;
	}
	return value.substr(begin, end - begin);
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.size(); ++index) {
		if (std::tolower(static_cast<unsigned char>(left[index])) != std::tolower(static_cast<unsigned char>(right[index]))) {
			return false;
		}
	}
	return true;
}

const nlohmann::json* FindMemberIgnoreCase(const nlohmann::json& object, std::string_view name) {
	if (!object.is_object()) {
		return nullptr;
	}
	for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
		if (EqualsIgnoreCase(iterator.key(), name)) {
			return &iterator.value();
		}
	}
	return nullptr;
}

void AddWarning(std::string& warning, const std::string& message) {
	if (!warning.empty()) {
		warning += "; ";
	}
	warning += message;
}

void ReadBool(const nlohmann::json& section, const char* key, bool& out, std::string& warning, const char* path) {
	const nlohmann::json* value = FindMemberIgnoreCase(section, key);
	if (value == nullptr) {
		return;
	}
	if (!value->is_boolean()) {
		AddWarning(warning, std::string(path) + "." + key + ": expected a boolean, keeping the default");
		return;
	}
	out = value->get<bool>();
}

void ReadInt(const nlohmann::json& section, const char* key, int& out, std::string& warning, const char* path) {
	const nlohmann::json* value = FindMemberIgnoreCase(section, key);
	if (value == nullptr) {
		return;
	}
	if (!value->is_number()) {
		AddWarning(warning, std::string(path) + "." + key + ": expected a number, keeping the default");
		return;
	}
	const double number = value->get<double>();
	if (!std::isfinite(number) || number < static_cast<double>(std::numeric_limits<int>::min())
		|| number > static_cast<double>(std::numeric_limits<int>::max())) {
		AddWarning(warning, std::string(path) + "." + key + ": out of range, keeping the default");
		return;
	}
	out = static_cast<int>(number);
}

void ReadDouble(const nlohmann::json& section, const char* key, double& out, std::string& warning, const char* path) {
	const nlohmann::json* value = FindMemberIgnoreCase(section, key);
	if (value == nullptr) {
		return;
	}
	if (!value->is_number()) {
		AddWarning(warning, std::string(path) + "." + key + ": expected a number, keeping the default");
		return;
	}
	const double number = value->get<double>();
	if (!std::isfinite(number)) {
		AddWarning(warning, std::string(path) + "." + key + ": not a finite number, keeping the default");
		return;
	}
	out = number;
}

void ReadString(const nlohmann::json& section, const char* key, std::string& out, std::string& warning, const char* path) {
	const nlohmann::json* value = FindMemberIgnoreCase(section, key);
	if (value == nullptr) {
		return;
	}
	if (!value->is_string()) {
		AddWarning(warning, std::string(path) + "." + key + ": expected a string, keeping the default");
		return;
	}
	out = value->get<std::string>();
}

std::string FormatLocal(std::time_t when, const char* format) {
	std::tm brokenDown {};
	localtime_r(&when, &brokenDown);
	char buffer[64] {};
	std::strftime(buffer, sizeof(buffer), format, &brokenDown);
	return buffer;
}

std::string ReplaceAll(std::string text, std::string_view needle, std::string_view replacement) {
	if (needle.empty()) {
		return text;
	}
	std::string::size_type position = 0;
	while ((position = text.find(needle, position)) != std::string::npos) {
		text.replace(position, needle.size(), replacement);
		position += replacement.size();
	}
	return text;
}

} // namespace

Config DefaultConfig() {
	return Config {};
}

void NormalizeConfig(Config& config) {
	config.general.minimumDemoDuration = std::max(0.0, config.general.minimumDemoDuration);
	config.general.autoCleanupIntervalMinutes = std::max(1, config.general.autoCleanupIntervalMinutes);
	config.general.autoCleanupFileAgeHours = std::max(1, config.general.autoCleanupFileAgeHours);
	config.autoRecord.idlePlayerCountThreshold = std::max(0, config.autoRecord.idlePlayerCountThreshold);
	config.autoRecord.idleTimeSeconds = std::max(0, config.autoRecord.idleTimeSeconds);
	config.s3.partSizeMb = std::clamp(config.s3.partSizeMb, 5, 5120);
	config.s3.retentionDays = std::max(1, config.s3.retentionDays);
	config.s3.prefix = NormalizeS3Prefix(config.s3.prefix);
	config.s3.endpoint = TrimWhitespace(config.s3.endpoint);
	config.s3.region = TrimWhitespace(config.s3.region);
	config.s3.bucket = TrimWhitespace(config.s3.bucket);
	config.s3.accessKey = TrimWhitespace(config.s3.accessKey);
	config.s3.secretKey = TrimWhitespace(config.s3.secretKey);
	config.http.transport = Lowercase(TrimWhitespace(config.http.transport));
	config.http.caFile = TrimWhitespace(config.http.caFile);
	if (config.http.transport != "auto" && config.http.transport != "steam" && config.http.transport != "curl") {
		config.http.transport = "auto";
	}
	// serverBoost is deliberately not clamped: the original's switch maps every value other than
	// 2/3 to the 25 MB limit (maxDiscordFileSizeMb), including out-of-range ones.
	// The original's config getter: a demo request system only makes sense with automatic
	// per-round recording, so enabling it forces both switches on.
	if (config.demoRequest.enabled) {
		config.autoRecord.enabled = true;
		config.autoRecord.cropRounds = true;
	}
}

int DiscordSettings::maxDiscordFileSizeMb() const {
	switch (serverBoost) {
	case 2: return 50;
	case 3: return 100;
	default: return 25;
	}
}

bool ParseConfig(std::string_view text, Config& out, std::string& warning) {
	warning.clear();
	out = DefaultConfig();
	nlohmann::json root;
	try {
		root = nlohmann::json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/true,
									 /*ignore_comments=*/true);
	} catch (const std::exception& exception) {
		warning = std::string("config.jsonc parse error: ") + exception.what();
		return false;
	}
	if (!root.is_object()) {
		warning = "config.jsonc must be a JSON object";
		return false;
	}

	const nlohmann::json* section = &root;
	const nlohmann::json* wrapper = FindMemberIgnoreCase(root, "K4GOTV");
	if (wrapper != nullptr) {
		if (wrapper->is_object()) {
			section = wrapper;
		} else {
			AddWarning(warning, "'K4GOTV' is not an object; reading the root object");
		}
	}

	Config parsed = DefaultConfig();
	if (const nlohmann::json* general = FindMemberIgnoreCase(*section, "General"); general != nullptr) {
		if (general->is_object()) {
			ReadDouble(*general, "MinimumDemoDuration", parsed.general.minimumDemoDuration, warning, "General");
			ReadBool(*general, "DeleteDemoAfterUpload", parsed.general.deleteDemoAfterUpload, warning, "General");
			ReadBool(*general, "DeleteZippedDemoAfterUpload", parsed.general.deleteZippedDemoAfterUpload, warning,
					 "General");
			ReadBool(*general, "DeleteEveryDemoFromServerAfterServerStart",
					 parsed.general.deleteEveryDemoFromServerAfterServerStart, warning, "General");
			ReadBool(*general, "LogUploads", parsed.general.logUploads, warning, "General");
			ReadBool(*general, "LogDeletions", parsed.general.logDeletions, warning, "General");
			ReadString(*general, "RegularFileNamingPattern", parsed.general.regularFileNamingPattern, warning, "General");
			ReadString(*general, "CropRoundsFileNamingPattern", parsed.general.cropRoundsFileNamingPattern, warning,
					   "General");
			ReadString(*general, "DemoDirectory", parsed.general.demoDirectory, warning, "General");
			ReadBool(*general, "AutoCleanupEnabled", parsed.general.autoCleanupEnabled, warning, "General");
			ReadInt(*general, "AutoCleanupIntervalMinutes", parsed.general.autoCleanupIntervalMinutes, warning,
					"General");
			ReadInt(*general, "AutoCleanupFileAgeHours", parsed.general.autoCleanupFileAgeHours, warning, "General");
		} else {
			AddWarning(warning, "'General' is not an object");
		}
	}

	if (const nlohmann::json* autoRecord = FindMemberIgnoreCase(*section, "AutoRecord"); autoRecord != nullptr) {
		if (autoRecord->is_object()) {
			ReadBool(*autoRecord, "Enabled", parsed.autoRecord.enabled, warning, "AutoRecord");
			ReadBool(*autoRecord, "CropRounds", parsed.autoRecord.cropRounds, warning, "AutoRecord");
			ReadBool(*autoRecord, "StopOnIdle", parsed.autoRecord.stopOnIdle, warning, "AutoRecord");
			ReadBool(*autoRecord, "RecordWarmup", parsed.autoRecord.recordWarmup, warning, "AutoRecord");
			ReadInt(*autoRecord, "IdlePlayerCountThreshold", parsed.autoRecord.idlePlayerCountThreshold, warning,
					"AutoRecord");
			ReadInt(*autoRecord, "IdleTimeSeconds", parsed.autoRecord.idleTimeSeconds, warning, "AutoRecord");
		} else {
			AddWarning(warning, "'AutoRecord' is not an object");
		}
	}

	if (const nlohmann::json* discord = FindMemberIgnoreCase(*section, "Discord"); discord != nullptr) {
		if (discord->is_object()) {
			ReadString(*discord, "WebhookURL", parsed.discord.webhookUrl, warning, "Discord");
			ReadString(*discord, "WebhookAvatar", parsed.discord.webhookAvatar, warning, "Discord");
			ReadBool(*discord, "WebhookUploadFile", parsed.discord.webhookUploadFile, warning, "Discord");
			ReadString(*discord, "WebhookName", parsed.discord.webhookName, warning, "Discord");
			ReadString(*discord, "EmbedTitle", parsed.discord.embedTitle, warning, "Discord");
			ReadString(*discord, "MessageText", parsed.discord.messageText, warning, "Discord");
			ReadInt(*discord, "ServerBoost", parsed.discord.serverBoost, warning, "Discord");
		} else {
			AddWarning(warning, "'Discord' is not an object");
		}
	}

	if (const nlohmann::json* demoRequest = FindMemberIgnoreCase(*section, "DemoRequest"); demoRequest != nullptr) {
		if (demoRequest->is_object()) {
			ReadBool(*demoRequest, "Enabled", parsed.demoRequest.enabled, warning, "DemoRequest");
			ReadBool(*demoRequest, "PrintAll", parsed.demoRequest.printAll, warning, "DemoRequest");
			ReadBool(*demoRequest, "DeleteUnused", parsed.demoRequest.deleteUnused, warning, "DemoRequest");
		} else {
			AddWarning(warning, "'DemoRequest' is not an object");
		}
	}

	if (const nlohmann::json* s3 = FindMemberIgnoreCase(*section, "S3"); s3 != nullptr) {
		if (s3->is_object()) {
			ReadString(*s3, "endpoint", parsed.s3.endpoint, warning, "S3");
			ReadString(*s3, "region", parsed.s3.region, warning, "S3");
			ReadString(*s3, "bucket", parsed.s3.bucket, warning, "S3");
			ReadString(*s3, "access_key", parsed.s3.accessKey, warning, "S3");
			ReadString(*s3, "secret_key", parsed.s3.secretKey, warning, "S3");
			ReadString(*s3, "prefix", parsed.s3.prefix, warning, "S3");
			ReadBool(*s3, "path_style", parsed.s3.pathStyle, warning, "S3");
			ReadInt(*s3, "part_size_mb", parsed.s3.partSizeMb, warning, "S3");
			ReadBool(*s3, "retention_enabled", parsed.s3.retentionEnabled, warning, "S3");
			ReadInt(*s3, "retention_days", parsed.s3.retentionDays, warning, "S3");
		} else {
			AddWarning(warning, "'S3' is not an object");
		}
	}
	// Surrounding whitespace in credentials/hostnames is always a copy-paste accident and would
	// otherwise make every request fail with an unexplained signature error.
	parsed.s3.endpoint = TrimWhitespace(parsed.s3.endpoint);
	parsed.s3.region = TrimWhitespace(parsed.s3.region);
	parsed.s3.bucket = TrimWhitespace(parsed.s3.bucket);
	parsed.s3.accessKey = TrimWhitespace(parsed.s3.accessKey);
	parsed.s3.secretKey = TrimWhitespace(parsed.s3.secretKey);

	if (const nlohmann::json* http = FindMemberIgnoreCase(*section, "Http"); http != nullptr) {
		if (http->is_object()) {
			ReadString(*http, "transport", parsed.http.transport, warning, "Http");
			ReadString(*http, "ca_file", parsed.http.caFile, warning, "Http");
		} else {
			AddWarning(warning, "'Http' is not an object");
		}
	}
	parsed.http.transport = Lowercase(TrimWhitespace(parsed.http.transport));
	parsed.http.caFile = TrimWhitespace(parsed.http.caFile);
	if (parsed.http.transport != "auto" && parsed.http.transport != "steam" && parsed.http.transport != "curl") {
		AddWarning(warning, "Http.transport must be auto, steam or curl; using auto");
		parsed.http.transport = "auto";
	}

	out = parsed;
	return true;
}

// --- time / formatting -----------------------------------------------------------------------

std::string LocalDate(std::time_t when) {
	return FormatLocal(when, "%Y-%m-%d");
}

std::string LocalClock(std::time_t when) {
	return FormatLocal(when, "%H:%M:%S");
}

std::string LocalFileTime(std::time_t when) {
	return FormatLocal(when, "%H-%M-%S");
}

std::string LocalTimestamp(std::time_t when) {
	return FormatLocal(when, "%Y%m%d_%H%M%S");
}

std::string LocalDateTime(std::time_t when) {
	return FormatLocal(when, "%Y-%m-%d %H:%M:%S");
}

std::string UtcIso8601(std::time_t when) {
	std::tm brokenDown {};
	gmtime_r(&when, &brokenDown);
	char buffer[64] {};
	std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &brokenDown);
	// .NET's DateTime.ToString("o") emits seven fractional digits; keep the same shape.
	return std::string(buffer) + ".0000000Z";
}

std::string FormatDuration(double seconds) {
	if (!(seconds > 0.0)) {
		return "00:00";
	}
	const auto total = static_cast<unsigned long long>(std::floor(seconds));
	// C#'s TimeSpan.ToString(@"mm\:ss") formats the *minute component* (0-59), so a 65 minute
	// demo prints "05:00", exactly like the original. Keep that parity.
	const unsigned long long minutes = (total / 60ull) % 60ull;
	const unsigned long long remainder = total % 60ull;
	char buffer[32] {};
	std::snprintf(buffer, sizeof(buffer), "%02llu:%02llu", minutes, remainder);
	return buffer;
}

std::time_t ParseIso8601(std::string_view text) {
	if (text.size() < 20) {
		return 0;
	}
	int year = 0;
	int month = 0;
	int day = 0;
	int hour = 0;
	int minute = 0;
	int second = 0;
	if (std::sscanf(std::string(text).c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) != 6) {
		return 0;
	}
	if (text.back() != 'Z') {
		return 0;
	}
	std::tm brokenDown {};
	brokenDown.tm_year = year - 1900;
	brokenDown.tm_mon = month - 1;
	brokenDown.tm_mday = day;
	brokenDown.tm_hour = hour;
	brokenDown.tm_min = minute;
	brokenDown.tm_sec = second;
	return timegm(&brokenDown);
}

// --- naming ----------------------------------------------------------------------------------

std::string BuildDemoFileName(std::string_view pattern, const NamingContext& context) {
	std::string name(pattern);
	name = ReplaceAll(std::move(name), "{fileName}", context.fileName);
	name = ReplaceAll(std::move(name), "{map}", context.map);
	name = ReplaceAll(std::move(name), "{date}", context.date);
	name = ReplaceAll(std::move(name), "{time}", context.time);
	name = ReplaceAll(std::move(name), "{timestamp}", context.timestamp);
	name = ReplaceAll(std::move(name), "{round}", std::to_string(context.round));
	name = ReplaceAll(std::move(name), "{playerCount}", std::to_string(context.playerCount));
	return name;
}

std::string AppendCollisionSuffix(std::string_view base, int suffix) {
	return std::string(base) + "_" + std::to_string(suffix);
}

// --- upload planning -------------------------------------------------------------------------

UploadPlan PlanUpload(uint64_t fileSize, uint64_t requestedPartSize, uint32_t maxParts) {
	if (maxParts == 0) {
		maxParts = 10000;
	}
	UploadPlan plan;
	plan.partSize = std::clamp(requestedPartSize, kMinimumPartSize, kMaximumPartSize);
	if (fileSize <= plan.partSize) {
		plan.useMultipart = false;
		plan.parts.push_back({0, fileSize});
		return plan;
	}

	for (;;) {
		const uint64_t partCount = (fileSize + plan.partSize - 1) / plan.partSize;
		if (partCount <= maxParts) {
			break;
		}
		// Raise the part size to the smallest whole MiB that fits the S3 part limit.
		const uint64_t needed = (fileSize + maxParts - 1) / maxParts;
		uint64_t rounded = ((needed + kMiB - 1) / kMiB) * kMiB;
		if (rounded <= plan.partSize) {
			rounded = plan.partSize + kMiB;
		}
		plan.partSize = std::min(rounded, kMaximumPartSize);
		if (plan.partSize >= kMaximumPartSize) {
			break;
		}
	}

	plan.useMultipart = true;
	uint64_t offset = 0;
	while (offset < fileSize) {
		const uint64_t remaining = fileSize - offset;
		const uint64_t length = std::min(plan.partSize, remaining);
		plan.parts.push_back({offset, length});
		offset += length;
	}
	return plan;
}

// --- retention -------------------------------------------------------------------------------

std::vector<std::string> SelectExpiredObjects(const std::vector<ObjectInfo>& objects, std::time_t now, int retentionDays) {
	const int days = std::max(1, retentionDays);
	const std::time_t cutoff = now - static_cast<std::time_t>(days) * 86400;
	std::vector<std::string> expired;
	for (const ObjectInfo& object : objects) {
		if (object.lastModified <= 0) {
			continue; // unknown age: never delete blindly
		}
		if (object.lastModified <= cutoff) {
			expired.push_back(object.key);
		}
	}
	return expired;
}

// --- S3 keys / URLs --------------------------------------------------------------------------

std::string NormalizeS3Prefix(std::string_view prefix) {
	std::size_t begin = 0;
	while (begin < prefix.size() && prefix[begin] == '/') {
		++begin;
	}
	std::string normalized(prefix.substr(begin));
	if (!normalized.empty() && normalized.back() != '/') {
		normalized.push_back('/');
	}
	return normalized;
}

std::string S3ObjectKey(std::string_view prefix, std::string_view fileName) {
	return NormalizeS3Prefix(prefix) + std::string(fileName);
}

std::string NormalizeEndpointHost(std::string_view endpoint) {
	std::string value(endpoint);
	if (value.find_first_of(" \t\r\n") != std::string::npos) {
		return "";
	}
	// Strip a scheme; the caller rejects non-HTTPS schemes (credentials must not travel in
	// plaintext).
	const std::size_t scheme = value.find("://");
	if (scheme != std::string::npos) {
		value.erase(0, scheme + 3);
	}
	while (!value.empty() && value.back() == '/') {
		value.pop_back();
	}
	if (value.empty() || value.find('/') != std::string::npos) {
		return "";
	}
	// HTTP clients omit a default port from the Host header; signing "host:443" then makes the
	// SigV4 canonical host differ from the request (seen live: an explicit :443 in the endpoint
	// produced AccessDenied). Normalize https:443 / http:80 away.
	auto hasDefaultPort = [&value](std::string_view port) {
		// `port` includes its colon (":443"), so it starts at size - port.size().
		if (value.size() <= port.size() || value.compare(value.size() - port.size(), port.size(), port) != 0) {
			return false;
		}
		const std::size_t colon = value.size() - port.size();
		if (value.front() == '[') {
			return colon >= 2 && value[colon - 1] == ']'; // [IPv6]:443
		}
		return value.find(':') == colon; // plain host:port (a raw IPv6 has more colons)
	};
	if (hasDefaultPort(":443")) {
		value.resize(value.size() - 4);
	} else if (hasDefaultPort(":80")) {
		value.resize(value.size() - 3);
	}
	return value;
}

bool EndpointIsHttps(std::string_view endpoint) {
	if (endpoint.empty()) {
		return false;
	}
	const std::size_t scheme = endpoint.find("://");
	if (scheme == std::string_view::npos) {
		return true; // a bare host means HTTPS
	}
	return endpoint.substr(0, scheme) == "https";
}

std::string S3ObjectUrl(std::string_view endpointHost, std::string_view bucket, std::string_view key, bool pathStyle) {
	std::string url = "https://";
	if (pathStyle) {
		url += std::string(endpointHost) + "/" + std::string(bucket) + "/" + UriEncode(key, false);
	} else {
		url += std::string(bucket) + "." + std::string(endpointHost) + "/" + UriEncode(key, false);
	}
	return url;
}

} // namespace k4
