// k4_gotv — AWS Signature Version 4 for S3.
//
// The canonical request / string-to-sign / signing-key derivation follow the AWS documentation
// ("Signature Calculation Examples", GET/PUT/lifecycle/list-objects vectors) and are unit-tested
// against those exact signatures. Only the s3 service is used here (single URI encoding; the
// double encoding of the canonical URI is for non-S3 services).
//
// The caller assembles the headers it will actually send (host, x-amz-content-sha256,
// x-amz-date, plus any x-amz-* header it wants signed) and passes the payload hash. The resulting
// Authorization header is then attached to the request; headers NOT listed in SignedHeaders may
// still be sent (Content-Type), but every x-amz-* header must be signed.

#pragma once

#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace k4 {

struct SigningInput {
	std::string method;		   // GET / PUT / POST / DELETE
	std::string canonicalUri;	 // already percent-encoded path, e.g. /bucket/demos%2Ffile.zip
	std::string canonicalQuery;	 // canonical (sorted + encoded) query string, "" when none
	// Header pairs to sign. Names are case-insensitive and values are trimmed by the signer;
	// the caller must send the same values (Host is set by the HTTP layer from the URL).
	std::vector<std::pair<std::string, std::string>> headers;
	std::string payloadHash;	// lowercase hex SHA-256 of the body (empty body: its SHA-256)
	std::string amzDate;		// 20130524T000000Z
	std::string region = "us-east-1";
	std::string service = "s3";
	std::string accessKey;
	std::string secretKey;
};

struct SigningResult {
	std::string canonicalRequest;
	std::string stringToSign;
	std::string signedHeaders;	   // "host;range;x-amz-content-sha256;x-amz-date"
	std::string signature;		   // lowercase hex
	std::string authorization;	   // full value for the Authorization header
};

SigningResult SignAwsV4(const SigningInput& input);

// RFC 3986 encoding for SigV4: unreserved characters (A-Z a-z 0-9 - . _ ~) pass through, every
// other byte becomes %XX (uppercase hex). '/' passes through when encodeSlash is false.
std::string UriEncode(std::string_view value, bool encodeSlash);

// Sorts by key then value, URI-encodes both, joins with '&'. Parameters with an empty value are
// rendered as "key=" (AWS canonical form).
std::string CanonicalQueryString(const std::vector<std::pair<std::string, std::string>>& parameters);

// First eight characters of an amzDate ("20130524T000000Z" -> "20130524").
std::string AwsDateStamp(std::string_view amzDate);

// UTC timestamp in the x-amz-date format.
std::string FormatAmzDate(std::time_t utcNow);

} // namespace k4
