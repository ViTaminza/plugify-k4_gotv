// k4_gotv — the small XML surface of the S3 REST API (ListObjectsV2, CreateMultipartUpload,
// CompleteMultipartUpload, error documents).
//
// The server answers with plain XML; a full XML parser is unnecessary. The helpers extract the
// tags the client needs, unescape the five predefined entities plus numeric character references,
// and never throw on malformed input (an empty result is reported instead).

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "k4_core.hpp"

namespace k4 {

struct ListObjectsResult {
	std::vector<ObjectInfo> objects;
	bool truncated = false;
	std::string nextContinuationToken;
	// Filled when the document is an <Error> document (or contains <Code>/<Message>).
	std::string errorCode;
	std::string errorMessage;
};

ListObjectsResult ParseListObjectsV2(std::string_view xml);

// Value of the first <tag>...</tag>; "" when absent. CDATA sections are returned verbatim.
std::string ParseXmlTag(std::string_view xml, std::string_view tag);

// <UploadId>...</UploadId> of CreateMultipartUpload.
std::string ParseUploadId(std::string_view xml);

struct S3ErrorInfo {
	std::string code;
	std::string message;
};

// <Code>/<Message> of an error document (empty when the body is not one).
S3ErrorInfo ParseS3Error(std::string_view body);

// CompleteMultipartUpload may answer HTTP 200 with an <Error> document; this detects it.
bool IsS3ErrorDocument(std::string_view body);

// Body of CompleteMultipartUpload; ETags are passed with their quotes (S3 returns them quoted).
std::string BuildCompleteMultipartXml(const std::vector<std::pair<int, std::string>>& parts);

std::string XmlUnescape(std::string_view text);

} // namespace k4
