// k4_gotv — the Discord payload and multipart body (see k4_discord_payload.hpp).

#include "k4_discord_payload.hpp"

#include <cstdio>
#include <vector>

namespace k4 {
namespace {

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

std::string JsonEscape(std::string_view value) {
	static constexpr char kHex[] = "0123456789abcdef";
	std::string out;
	out.reserve(value.size() + 8);
	for (const unsigned char character : value) {
		switch (character) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (character < 0x20) {
				out += "\\u00";
				out.push_back(kHex[character >> 4]);
				out.push_back(kHex[character & 0x0f]);
			} else {
				out.push_back(static_cast<char>(character));
			}
		}
	}
	return out;
}

std::string DiscordDownloadLinks(std::string_view url, std::string_view objectKey) {
	if (url.empty()) {
		return "";
	}
	std::string links = "**S3:** [Click Here](";
	links += url;
	links += ")\n**Key:** `";
	links += objectKey;
	links += "`";
	return links;
}

std::string BuildDiscordPayload(std::string_view templateText, const DiscordSettings& settings,
								const DiscordPayloadContext& context) {
	const unsigned long long fileSizeMb = context.fileSizeBytes / (1024ull * 1024ull);
	const std::string warning = fileSizeMb > static_cast<unsigned long long>(settings.maxDiscordFileSizeMb())
									? "⚠️ File size (" + std::to_string(fileSizeMb) + "MB) exceeds Discord limit."
									: "";

	// The original's replacement chain, same placeholders and same values.
	const std::vector<std::pair<std::string, std::string>> replacements = {
		{"{webhook_name}", settings.webhookName},
		{"{webhook_avatar}", settings.webhookAvatar},
		{"{message_text}", settings.messageText},
		{"{embed_title}", settings.embedTitle},
		{"{map}", context.map},
		{"{date}", context.date},
		{"{time}", context.time},
		{"{timedate}", context.timedate},
		{"{length}", context.length},
		{"{round}", std::to_string(context.round)},
		{"{download_links}", context.downloadLinks},
		{"{requester_name}", context.requesterNames},
		{"{requester_steamid}", context.requesterSteamIds},
		{"{requester_both}", context.requesterBoth},
		{"{requester_count}", std::to_string(context.requesterCount)},
		{"{player_count}", std::to_string(context.playerCount)},
		{"{server_name}", context.serverName},
		{"{fileName}", context.fileName},
		{"{iso_timestamp}", context.isoTimestamp},
		{"{file_size_warning}", warning},
		{"{fileSizeInKB}", std::to_string(context.fileSizeBytes / 1024ull)},
	};

	std::string payload(templateText);
	for (const auto& [placeholder, value] : replacements) {
		payload = ReplaceAll(std::move(payload), placeholder, JsonEscape(value));
	}
	return payload;
}

std::string BuildDiscordMultipart(std::string_view boundary, std::string_view payload, std::string_view fileName,
								  std::string_view fileData) {
	std::string body;
	body.reserve(payload.size() + fileData.size() + fileName.size() + boundary.size() * 6 + 256);
	body += "--";
	body += boundary;
	body += "\r\nContent-Disposition: form-data; name=\"payload_json\"\r\nContent-Type: application/json\r\n\r\n";
	body += payload;
	body += "\r\n--";
	body += boundary;
	body += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"";
	body += fileName;
	body += "\"\r\nContent-Type: application/zip\r\n\r\n";
	body += fileData;
	body += "\r\n--";
	body += boundary;
	body += "--\r\n";
	return body;
}

std::string DiscordMultipartContentType(std::string_view boundary) {
	return "multipart/form-data; boundary=" + std::string(boundary);
}

} // namespace k4
