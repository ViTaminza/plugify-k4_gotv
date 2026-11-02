// k4_gotv — the signed S3 request builder, shared by the plugin (k4_s3.cpp) and the host-side
// probe tool (tools/s3probe.cpp). Engine-free: it only needs the parsed S3 settings and the
// endpoint host.
//
// The request is built once, here, so the probe sends byte-for-byte the same URL, headers and
// payload hash the plugin sends (the plugin's only extra is the x-amz-meta-k4-upload-id header
// passed in by the multipart Create step).

#pragma once

#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "k4_core.hpp"

namespace k4 {

struct S3SignedRequest {
	std::string url;
	// Authorization, x-amz-content-sha256, x-amz-date, then the extra (signed) headers.
	std::vector<std::pair<std::string, std::string>> headers;
	std::string contentType;
	std::string body;
};

// method: GET / HEAD / POST / PUT / DELETE. `key` is the object key ("" for bucket-level
// requests); bucketLevel selects a request on the bucket itself (ListObjectsV2). The endpoint
// host must already be normalized (NormalizeEndpointHost). `now` is the signing time (tests pin
// it; production uses the current time).
S3SignedRequest BuildS3SignedRequest(const S3Settings& settings, std::string_view host, const std::string& method,
									const std::string& key, bool bucketLevel,
									const std::vector<std::pair<std::string, std::string>>& query, std::string body,
									const std::string& contentType,
									const std::vector<std::pair<std::string, std::string>>& extraHeaders = {},
									std::time_t now = std::time(nullptr));

// Same request, but the caller already has the SHA-256 of `body`. The uploader computes it on its
// file worker while reading the part, so a 64 MiB body is not hashed on the game thread; the hash
// must be the one of exactly this body. An empty `payloadHash` falls back to hashing here.
S3SignedRequest BuildS3SignedRequest(const S3Settings& settings, std::string_view host, const std::string& method,
									const std::string& key, bool bucketLevel,
									const std::vector<std::pair<std::string, std::string>>& query, std::string body,
									const std::string& contentType, const std::string& payloadHash,
									const std::vector<std::pair<std::string, std::string>>& extraHeaders = {},
									std::time_t now = std::time(nullptr));

} // namespace k4
