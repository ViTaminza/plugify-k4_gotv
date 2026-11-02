// k4_gotv — text helpers (see k4_text.hpp).

#include "k4_text.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace k4 {
namespace {

struct ColorTag {
	std::string_view tag;
	std::string_view bytes;
};

// Exactly pisexcompat's table (libs/pisexcompat/src/phrases.cpp): the order matters only for
// overlapping tags, and {LIGHTPURPLE} must be replaced before {PURPLE} because the shorter tag is
// a suffix of the longer one. The wrapper iterates the same order.
constexpr std::array<ColorTag, 17> kColorTags = {{{"{DEFAULT}", "\x01"},
												  {"{WHITE}", "\x01"},
												  {"{RED}", "\x02"},
												  {"{LIGHTPURPLE}", "\x03"},
												  {"{GREEN}", "\x04"},
												  {"{LIME}", "\x05"},
												  {"{LIGHTGREEN}", "\x06"},
												  {"{DARKRED}", "\x07"},
												  {"{GRAY}", "\x08"},
												  {"{LIGHTOLIVE}", "\x09"},
												  {"{OLIVE}", "\x10"},
												  {"{LIGHTBLUE}", "\x0B"},
												  {"{BLUE}", "\x0C"},
												  {"{PURPLE}", "\x0E"},
												  {"{LIGHTRED}", "\x0F"},
												  {"{GRAYBLUE}", "\x0A"},
												  {"\\n", "\xe2\x80\xa9"}}};

std::string ToLowerAscii(std::string_view text) {
	std::string lower;
	lower.reserve(text.size());
	for (const char character : text) {
		lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
	}
	return lower;
}

bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) {
	if (text.size() < prefix.size()) {
		return false;
	}
	for (std::size_t index = 0; index < prefix.size(); ++index) {
		if (std::tolower(static_cast<unsigned char>(text[index]))
			!= std::tolower(static_cast<unsigned char>(prefix[index]))) {
			return false;
		}
	}
	return true;
}

// A Discord webhook URL is a credential even without a query string. Replace the path of every
// discord.com/discordapp.com URL with "/..." (the query masker below cannot help there, because
// the token is part of the path).
void MaskDiscordUrls(std::string& text) {
	std::size_t position = 0;
	while (true) {
		const std::size_t scheme = text.find("://", position);
		if (scheme == std::string::npos) {
			return;
		}
		const std::size_t hostBegin = scheme + 3;
		const std::size_t hostEnd = text.find_first_of("/? \t\r\n\"'", hostBegin);
		const std::size_t hostLength = (hostEnd == std::string::npos ? text.size() : hostEnd) - hostBegin;
		const std::string host = ToLowerAscii(std::string_view(text).substr(hostBegin, hostLength));
		if (host.size() < 9 || host.find("discord") == std::string::npos) {
			position = hostBegin;
			continue;
		}
		// Only the slash that follows the host belongs to this URL. A query directly after
		// the host means there is no path to hide (the query masker handles it), and a later
		// slash may belong to the next URL in the message.
		const std::size_t pathBegin =
			(hostEnd != std::string::npos && hostEnd < text.size() && text[hostEnd] == '/') ? hostEnd
																							 : std::string::npos;
		if (pathBegin == std::string::npos) {
			position = hostBegin;
			continue;
		}
		std::size_t pathEnd = pathBegin;
		while (pathEnd < text.size()) {
			const unsigned char character = static_cast<unsigned char>(text[pathEnd]);
			if (std::isspace(character) != 0 || character == '"' || character == '\'' || character == ')'
				|| character == ']' || character == ',') {
				break;
			}
			++pathEnd;
		}
		text.replace(pathBegin, pathEnd - pathBegin, "/...");
		position = pathBegin + 4;
	}
}

// Replaces the value of every `name=value` occurrence (case-insensitive name), stopping at '&',
// whitespace, a quote or a closing parenthesis. `name` includes its separator, e.g.
// "x-amz-signature=".
void MaskQueryValue(std::string& text, std::string_view name) {
	std::size_t position = 0;
	while (position < text.size()) {
		const std::size_t found = [&]() -> std::size_t {
			for (std::size_t index = position; index + name.size() <= text.size(); ++index) {
				if (StartsWithIgnoreCase(std::string_view(text).substr(index), name)) {
					return index;
				}
			}
			return std::string::npos;
		}();
		if (found == std::string::npos) {
			return;
		}
		std::size_t valueBegin = found + name.size();
		std::size_t valueEnd = valueBegin;
		while (valueEnd < text.size()) {
			const unsigned char character = static_cast<unsigned char>(text[valueEnd]);
			if (character == '&' || std::isspace(character) != 0 || character == '"' || character == '\''
				|| character == ')' || character == ']' || character == ',') {
				break;
			}
			++valueEnd;
		}
		if (valueEnd > valueBegin) {
			text.replace(valueBegin, valueEnd - valueBegin, "***");
			position = valueBegin + 3;
		} else {
			position = valueBegin;
		}
	}
}

} // namespace

std::string Colorize(std::string_view text) {
	std::string out(text);
	for (const ColorTag& color : kColorTags) {
		std::size_t position = 0;
		while ((position = out.find(color.tag, position)) != std::string::npos) {
			out.replace(position, color.tag.size(), color.bytes);
			position += color.bytes.size();
		}
	}
	return out;
}

std::string StripColorTags(std::string_view text) {
	std::string out(text);
	for (const ColorTag& color : kColorTags) {
		const std::string_view replacement = color.tag == "\\n" ? std::string_view("\n") : std::string_view();
		std::size_t position = 0;
		while ((position = out.find(color.tag, position)) != std::string::npos) {
			out.replace(position, color.tag.size(), replacement);
			position += replacement.size();
		}
	}
	return out;
}

std::string NormalizeLanguage(std::string_view name) {
	const std::string lower = ToLowerAscii(name);
	if (lower == "russian" || lower == "ru") return "ru";
	if (lower == "english" || lower == "en") return "en";
	if (lower == "ukrainian" || lower == "ua" || lower == "uk") return "ua";
	if (lower == "portuguese" || lower == "pt-br" || lower == "brazilian") return "pt-BR";
	if (lower == "spanish" || lower == "es") return "es";
	if (lower == "german" || lower == "de") return "de";
	if (lower == "french" || lower == "fr") return "fr";
	if (lower == "polish" || lower == "pl") return "pl";
	if (lower == "turkish" || lower == "tr") return "tr";
	if (lower == "schinese" || lower == "zh-cn") return "zh-CN";
	return std::string(name);
}

std::string RedactUrl(std::string_view url) {
	const std::size_t scheme = url.find("://");
	if (scheme == std::string_view::npos) {
		return "...";
	}
	const std::size_t path = url.find('/', scheme + 3);
	if (path == std::string_view::npos) {
		return std::string(url);
	}
	return std::string(url.substr(0, path)) + "/...";
}

std::string RedactSensitive(std::string text) {
	MaskDiscordUrls(text);
	// AWS SigV4 query authentication (a presigned URL carries the credential + signature).
	MaskQueryValue(text, "x-amz-signature=");
	MaskQueryValue(text, "x-amz-credential=");
	MaskQueryValue(text, "x-amz-security-token=");
	// A hand-written error or a diagnostic that echoes a request URI.
	MaskQueryValue(text, "password=");
	MaskQueryValue(text, "passwd=");
	MaskQueryValue(text, "pass=");
	MaskQueryValue(text, "token=");
	MaskQueryValue(text, "secret_key=");
	MaskQueryValue(text, "access_key=");
	return text;
}

} // namespace k4
