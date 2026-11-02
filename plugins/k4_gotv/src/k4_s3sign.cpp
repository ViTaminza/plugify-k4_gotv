// k4_gotv — shared signed request builder (see k4_s3sign.hpp). Moved verbatim from the private
// S3Client::Impl::BuildRequest so the plugin and the host-side probe produce identical requests.

#include "k4_s3sign.hpp"

#include "k4_crypto.hpp"
#include "k4_sigv4.hpp"

#include <ctime>
#include <utility>

namespace k4 {
namespace {

S3SignedRequest BuildSignedRequest(const S3Settings& settings, std::string_view host, const std::string& method,
								   const std::string& key, bool bucketLevel,
								   const std::vector<std::pair<std::string, std::string>>& query, std::string body,
								   const std::string& contentType,
								   const std::vector<std::pair<std::string, std::string>>& extraHeaders,
								   std::time_t now, std::string_view payloadHash) {
	S3SignedRequest request;
	request.contentType = contentType;
	request.body = std::move(body);

	std::string canonicalUri;
	if (bucketLevel) {
		canonicalUri = settings.pathStyle ? "/" + settings.bucket : "/";
	} else {
		canonicalUri = "/" + UriEncode(key, false);
		if (settings.pathStyle) {
			canonicalUri = "/" + settings.bucket + canonicalUri;
		}
	}
	const std::string requestHost = settings.pathStyle ? std::string(host) : settings.bucket + "." + std::string(host);

	const std::string queryString = CanonicalQueryString(query);
	request.url = "https://" + requestHost + canonicalUri;
	if (!queryString.empty()) {
		request.url += "?" + queryString;
	}

	// The uploader passes the hash of the body it read on its worker; an empty value (every small
	// control request, the probe) is hashed here.
	const std::string hash = payloadHash.empty() ? Sha256Hex(request.body) : std::string(payloadHash);
	const std::string amzDate = FormatAmzDate(now);

	SigningInput signing;
	signing.method = method;
	signing.canonicalUri = canonicalUri;
	signing.canonicalQuery = queryString;
	signing.headers = {{"host", requestHost}, {"x-amz-content-sha256", hash}, {"x-amz-date", amzDate}};
	for (const auto& [name, value] : extraHeaders) {
		signing.headers.emplace_back(name, value);
	}
	signing.payloadHash = hash;
	signing.amzDate = amzDate;
	signing.region = settings.region;
	signing.service = "s3";
	signing.accessKey = settings.accessKey;
	signing.secretKey = settings.secretKey;
	const SigningResult signedRequest = SignAwsV4(signing);

	request.headers = {{"Authorization", signedRequest.authorization},
					   {"x-amz-content-sha256", hash},
					   {"x-amz-date", amzDate}};
	for (const auto& [name, value] : extraHeaders) {
		request.headers.emplace_back(name, value);
	}
	return request;
}

} // namespace

S3SignedRequest BuildS3SignedRequest(const S3Settings& settings, std::string_view host, const std::string& method,
									const std::string& key, bool bucketLevel,
									const std::vector<std::pair<std::string, std::string>>& query, std::string body,
									const std::string& contentType,
									const std::vector<std::pair<std::string, std::string>>& extraHeaders,
									std::time_t now) {
	return BuildSignedRequest(settings, host, method, key, bucketLevel, query, std::move(body), contentType,
							  extraHeaders, now, {});
}

S3SignedRequest BuildS3SignedRequest(const S3Settings& settings, std::string_view host, const std::string& method,
									const std::string& key, bool bucketLevel,
									const std::vector<std::pair<std::string, std::string>>& query, std::string body,
									const std::string& contentType, const std::string& payloadHash,
									const std::vector<std::pair<std::string, std::string>>& extraHeaders,
									std::time_t now) {
	return BuildSignedRequest(settings, host, method, key, bucketLevel, query, std::move(body), contentType,
							  extraHeaders, now, payloadHash);
}

} // namespace k4
