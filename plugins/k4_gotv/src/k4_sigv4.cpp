// k4_gotv — AWS Signature Version 4 for S3 (see k4_sigv4.hpp).
//
// Follows the AWS "Signature Calculation Examples": canonical request, string to sign, the four
// derived signing keys and the Authorization header. The S3 service uses one URI-encoding pass
// (the double pass is for services other than S3), so canonicalUri is encoded by the caller with
// UriEncode(..., false) and the query with encodeSlash=true.

#include "k4_sigv4.hpp"

#include "k4_crypto.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace k4 {
namespace {

bool IsUnreserved(unsigned char character) {
	return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9')
		   || character == '-' || character == '.' || character == '_' || character == '~';
}

std::string NormalizeHeaderValue(std::string_view value) {
	std::string out;
	out.reserve(value.size());
	bool pendingSpace = false;
	for (const unsigned char character : value) {
		if (character == ' ' || character == '\t') {
			pendingSpace = !out.empty();
			continue;
		}
		if (pendingSpace) {
			out.push_back(' ');
			pendingSpace = false;
		}
		out.push_back(static_cast<char>(character));
	}
	return out; // trailing whitespace is dropped because pendingSpace is never flushed at the end
}

std::string Lowercase(std::string_view value) {
	std::string out(value);
	for (char& character : out) {
		character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	}
	return out;
}

std::string_view AsStringView(const Sha256Digest& digest) {
	return {reinterpret_cast<const char*>(digest.data()), digest.size()};
}

} // namespace

std::string UriEncode(std::string_view value, bool encodeSlash) {
	static constexpr char kHex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(value.size() + value.size() / 4);
	for (const unsigned char character : value) {
		if (IsUnreserved(character)) {
			out.push_back(static_cast<char>(character));
		} else if (character == '/' && !encodeSlash) {
			out.push_back('/');
		} else {
			out.push_back('%');
			out.push_back(kHex[character >> 4]);
			out.push_back(kHex[character & 0x0f]);
		}
	}
	return out;
}

std::string CanonicalQueryString(const std::vector<std::pair<std::string, std::string>>& parameters) {
	std::vector<std::pair<std::string, std::string>> sorted;
	sorted.reserve(parameters.size());
	for (const auto& [key, value] : parameters) {
		sorted.emplace_back(UriEncode(key, true), UriEncode(value, true));
	}
	std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
		if (left.first != right.first) {
			return left.first < right.first;
		}
		return left.second < right.second;
	});
	std::string out;
	for (const auto& [key, value] : sorted) {
		if (!out.empty()) {
			out.push_back('&');
		}
		out += key;
		out.push_back('=');
		out += value;
	}
	return out;
}

std::string AwsDateStamp(std::string_view amzDate) {
	if (amzDate.size() < 8) {
		return std::string(amzDate);
	}
	return std::string(amzDate.substr(0, 8));
}

std::string FormatAmzDate(std::time_t utcNow) {
	std::tm brokenDown {};
	gmtime_r(&utcNow, &brokenDown);
	char buffer[32] {};
	std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", &brokenDown);
	return buffer;
}

SigningResult SignAwsV4(const SigningInput& input) {
	SigningResult result;

	std::vector<std::pair<std::string, std::string>> headers;
	headers.reserve(input.headers.size());
	for (const auto& [name, value] : input.headers) {
		headers.emplace_back(Lowercase(name), NormalizeHeaderValue(value));
	}
	std::sort(headers.begin(), headers.end(),
			  [](const auto& left, const auto& right) { return left.first < right.first; });

	std::string canonicalHeaders;
	for (const auto& [name, value] : headers) {
		canonicalHeaders += name;
		canonicalHeaders.push_back(':');
		canonicalHeaders += value;
		canonicalHeaders.push_back('\n');
	}
	for (std::size_t index = 0; index < headers.size(); ++index) {
		if (index != 0) {
			result.signedHeaders.push_back(';');
		}
		result.signedHeaders += headers[index].first;
	}

	result.canonicalRequest = input.method;
	result.canonicalRequest.push_back('\n');
	result.canonicalRequest += input.canonicalUri;
	result.canonicalRequest.push_back('\n');
	result.canonicalRequest += input.canonicalQuery;
	result.canonicalRequest.push_back('\n');
	result.canonicalRequest += canonicalHeaders;
	result.canonicalRequest.push_back('\n');
	result.canonicalRequest += result.signedHeaders;
	result.canonicalRequest.push_back('\n');
	result.canonicalRequest += input.payloadHash;

	const std::string scope = AwsDateStamp(input.amzDate) + "/" + input.region + "/" + input.service + "/aws4_request";
	result.stringToSign = "AWS4-HMAC-SHA256\n" + input.amzDate + "\n" + scope + "\n" + Sha256Hex(result.canonicalRequest);

	const std::string dateKey = "AWS4" + input.secretKey;
	const Sha256Digest dateSigning = HmacSha256(dateKey, AwsDateStamp(input.amzDate));
	const Sha256Digest regionSigning = HmacSha256(AsStringView(dateSigning), input.region);
	const Sha256Digest serviceSigning = HmacSha256(AsStringView(regionSigning), input.service);
	const Sha256Digest signingKey = HmacSha256(AsStringView(serviceSigning), "aws4_request");
	result.signature = HmacSha256Hex(AsStringView(signingKey), result.stringToSign);

	result.authorization = "AWS4-HMAC-SHA256 Credential=" + input.accessKey + "/" + scope + ", SignedHeaders="
						   + result.signedHeaders + ", Signature=" + result.signature;
	return result;
}

} // namespace k4
