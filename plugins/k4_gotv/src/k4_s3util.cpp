// k4_gotv — the S3 XML surface (see k4_s3util.hpp).

#include "k4_s3util.hpp"

#include <cstdlib>

namespace k4 {
namespace {

bool IsTagNameEnd(char character) {
	return character == '>' || character == ' ' || character == '\t' || character == '\r' || character == '\n'
		   || character == '/';
}

// Finds the content of the first <tag>; `<tag ...>` attributes are skipped up to '>'.
std::string ExtractTag(std::string_view xml, std::string_view tag) {
	std::string opening = "<" + std::string(tag);
	std::size_t position = 0;
	while ((position = xml.find(opening, position)) != std::string_view::npos) {
		const std::size_t nameEnd = position + opening.size();
		if (nameEnd < xml.size() && IsTagNameEnd(xml[nameEnd])) {
			const std::size_t contentStart = xml.find('>', nameEnd);
			if (contentStart == std::string_view::npos) {
				return "";
			}
			const std::string closing = "</" + std::string(tag) + ">";
			const std::size_t contentEnd = xml.find(closing, contentStart + 1);
			if (contentEnd == std::string_view::npos) {
				return "";
			}
			return std::string(xml.substr(contentStart + 1, contentEnd - contentStart - 1));
		}
		position = nameEnd;
	}
	return "";
}

std::string Trim(std::string value) {
	const std::size_t first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) {
		return "";
	}
	const std::size_t last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

} // namespace

std::string XmlUnescape(std::string_view text) {
	std::string out;
	out.reserve(text.size());
	std::size_t position = 0;
	while (position < text.size()) {
		if (text[position] != '&') {
			out.push_back(text[position++]);
			continue;
		}
		const std::size_t semicolon = text.find(';', position + 1);
		if (semicolon == std::string_view::npos) {
			out.push_back(text[position++]);
			continue;
		}
		const std::string_view entity = text.substr(position + 1, semicolon - position - 1);
		if (entity == "amp") {
			out.push_back('&');
		} else if (entity == "lt") {
			out.push_back('<');
		} else if (entity == "gt") {
			out.push_back('>');
		} else if (entity == "quot") {
			out.push_back('"');
		} else if (entity == "apos") {
			out.push_back('\'');
		} else if (entity.size() >= 2 && entity[0] == '#') {
			long codePoint = 0;
			bool valid = true;
			if (entity[1] == 'x' || entity[1] == 'X') {
				if (entity.size() <= 2) {
					valid = false;
				} else {
					char* end = nullptr;
					const std::string digits(entity.substr(2));
					codePoint = std::strtol(digits.c_str(), &end, 16);
					valid = end != nullptr && end != digits.c_str() && *end == '\0';
				}
			} else {
				char* end = nullptr;
				const std::string digits(entity.substr(1));
				codePoint = std::strtol(digits.c_str(), &end, 10);
				valid = end != nullptr && end != digits.c_str() && *end == '\0';
			}
			if (!valid || codePoint < 0 || codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
				out.append(text.substr(position, semicolon - position + 1));
				position = semicolon + 1;
				continue;
			}
			// UTF-8 encode the code point (S3 keys can contain any character).
			const auto value = static_cast<unsigned long>(codePoint);
			if (value < 0x80ul) {
				out.push_back(static_cast<char>(value));
			} else if (value < 0x800ul) {
				out.push_back(static_cast<char>(0xc0u | (value >> 6)));
				out.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
			} else if (value < 0x10000ul) {
				out.push_back(static_cast<char>(0xe0u | (value >> 12)));
				out.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3fu)));
				out.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
			} else if (value < 0x110000ul) {
				out.push_back(static_cast<char>(0xf0u | (value >> 18)));
				out.push_back(static_cast<char>(0x80u | ((value >> 12) & 0x3fu)));
				out.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3fu)));
				out.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
			}
		} else {
			out.append(text.substr(position, semicolon - position + 1));
		}
		position = semicolon + 1;
	}
	return out;
}

std::string ParseXmlTag(std::string_view xml, std::string_view tag) {
	std::string content = ExtractTag(xml, tag);
	const std::size_t first = content.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) {
		return "";
	}
	const std::size_t last = content.find_last_not_of(" \t\r\n");
	content = content.substr(first, last - first + 1);
	if (content.rfind("<![CDATA[", 0) == 0 && content.size() >= 12 && content.compare(content.size() - 3, 3, "]]>") == 0) {
		return content.substr(9, content.size() - 12);
	}
	return content;
}

S3ErrorInfo ParseS3Error(std::string_view body) {
	S3ErrorInfo info;
	if (!IsS3ErrorDocument(body)) {
		return info;
	}
	info.code = XmlUnescape(ParseXmlTag(body, "Code"));
	info.message = XmlUnescape(ParseXmlTag(body, "Message"));
	return info;
}

bool IsS3ErrorDocument(std::string_view body) {
	return body.find("<Error") != std::string_view::npos;
}

std::string ParseUploadId(std::string_view xml) {
	return XmlUnescape(ParseXmlTag(xml, "UploadId"));
}

ListObjectsResult ParseListObjectsV2(std::string_view xml) {
	ListObjectsResult result;
	if (IsS3ErrorDocument(xml)) {
		const S3ErrorInfo error = ParseS3Error(xml);
		result.errorCode = error.code;
		result.errorMessage = error.message;
		return result;
	}

	constexpr std::string_view contentsOpen = "<Contents>";
	constexpr std::string_view contentsClose = "</Contents>";
	std::size_t position = 0;
	while ((position = xml.find(contentsOpen, position)) != std::string_view::npos) {
		const std::size_t contentStart = position + contentsOpen.size();
		const std::size_t contentEnd = xml.find(contentsClose, contentStart);
		if (contentEnd == std::string_view::npos) {
			break;
		}
		const std::string_view block = xml.substr(contentStart, contentEnd - contentStart);
		ObjectInfo object;
		object.key = XmlUnescape(ParseXmlTag(block, "Key"));
		object.lastModified = ParseIso8601(ParseXmlTag(block, "LastModified"));
		if (!object.key.empty()) {
			result.objects.push_back(std::move(object));
		}
		position = contentEnd + contentsClose.size();
	}

	result.truncated = Trim(ParseXmlTag(xml, "IsTruncated")) == "true";
	result.nextContinuationToken = XmlUnescape(ParseXmlTag(xml, "NextContinuationToken"));
	return result;
}

std::string BuildCompleteMultipartXml(const std::vector<std::pair<int, std::string>>& parts) {
	std::string xml = "<CompleteMultipartUpload>";
	for (const auto& [number, etag] : parts) {
		xml += "<Part><PartNumber>";
		xml += std::to_string(number);
		xml += "</PartNumber><ETag>";
		xml += etag;
		xml += "</ETag></Part>";
	}
	xml += "</CompleteMultipartUpload>";
	return xml;
}

} // namespace k4
