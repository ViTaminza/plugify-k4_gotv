// k4_gotv — the Discord payload (resources/payload.json of the original) and the multipart body
// used when the finished .zip is attached.
//
// The original replaced every {placeholder} in the template text with a plain string and POSTed
// the result as application/json. The template puts every placeholder inside a JSON string, so
// values are JSON-escaped here first (the original did not escape them; a server name with a quote
// would have produced an invalid payload). The set of placeholders and their values is the
// original's, field for field.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "k4_core.hpp"

namespace k4 {

struct DiscordPayloadContext {
	std::string map;
	std::string date;	  // yyyy-MM-dd
	std::string time;	  // HH:mm:ss
	std::string timedate; // yyyy-MM-dd HH:mm:ss
	std::string length;	  // mm:ss
	int round = 1;
	int playerCount = 0;
	std::string requesterNames;
	std::string requesterSteamIds;
	std::string requesterBoth;
	int requesterCount = 0;
	std::string fileName;
	std::string serverName;
	std::string isoTimestamp;
	std::string downloadLinks; // raw markdown text ("No external uploads" when empty)
	uint64_t fileSizeBytes = 0;
};

// The original's replacement table. Returns the final JSON text (not validated).
std::string BuildDiscordPayload(std::string_view templateText, const DiscordSettings& settings,
								const DiscordPayloadContext& context);

// "**S3:** [Click Here](url)\n**Key:** `key`"; an empty url yields "" (the caller then uses
// "No external uploads" like the original did when no upload link existed).
std::string DiscordDownloadLinks(std::string_view url, std::string_view objectKey);

std::string JsonEscape(std::string_view value);

// multipart/form-data body with payload_json + file, byte-for-byte what HttpClient's
// MultipartFormDataContent produced in the original.
std::string BuildDiscordMultipart(std::string_view boundary, std::string_view payload,
								  std::string_view fileName, std::string_view fileData);
std::string DiscordMultipartContentType(std::string_view boundary);

} // namespace k4
