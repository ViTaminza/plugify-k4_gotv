// k4_gotv — tests for the engine-free core.
//
// Covers the pieces whose correctness cannot be checked on a live server cheaply:
//   * SHA-256 / HMAC-SHA256 (RFC 4231 vectors) / CRC-32,
//   * AWS Signature V4 against the four official S3 signature-calculation examples,
//   * SigV4 URI/query canonicalisation,
//   * config.jsonc parsing (comments, defaults, clamping, the DemoRequest rule),
//   * demo file naming and collision suffixes,
//   * time formatting and the C# TimeSpan mm:ss format,
//   * multipart part planning (threshold, S3 5 MiB minimum, 10000-part limit),
//   * retention selection,
//   * ListObjectsV2 / CompleteMultipartUpload / error XML,
//   * the ZIP container (local header fields, CRC, EOCD),
//   * the Discord payload replacement + multipart body,
//   * the `k4-gotv` table statements against SQLite, including the idempotent INSERT.
//
// Run from ./test.sh (build/tests/k4_gotv_tests). No engine is involved.

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "ent_handle.hpp"
#include "k4_core.hpp"
#include "k4_crypto.hpp"
#include "k4_curl.hpp"
#include "k4_discord_payload.hpp"
#include "k4_kv1.hpp"
#include "k4_s3sign.hpp"
#include "k4_s3util.hpp"
#include "k4_sigv4.hpp"
#include "k4_text.hpp"
#include "k4_zip.hpp"
#include "map_gate.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::cerr << file << ":" << line << ": CHECK failed: " << expression << "\n";
	}
}

#define CHECK(cond) Check((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(actual, expected)                                                                                     \
	Check((actual) == (expected), #actual " == " #expected, __FILE__, __LINE__)

std::string Escaped(std::string_view value) {
	std::string out;
	for (const char c : value) {
		if (c == '\n') {
			out += "\\n";
		} else if (c == '\r') {
			out += "\\r";
		} else {
			out.push_back(c);
		}
	}
	return out;
}

void CheckString(const std::string& actual, const std::string& expected, const char* what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::cerr << "string mismatch (" << what << ")\n  actual:   [" << Escaped(actual) << "]\n  expected: ["
				  << Escaped(expected) << "]\n";
	}
}

// --- crypto ----------------------------------------------------------------------------------

void TestCrypto() {
	CHECK_EQ(k4::Sha256Hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	CHECK_EQ(k4::Sha256Hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	CHECK_EQ(k4::Sha256Hex("The quick brown fox jumps over the lazy dog"),
			 std::string("d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"));
	CHECK_EQ(k4::Sha256Hex("Welcome to Amazon S3."),
			 std::string("44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072"));

	// RFC 4231 test cases 1, 2, 3 and 6 (key longer than the block size).
	CHECK_EQ(k4::HmacSha256Hex(std::string(20, '\x0b'), "Hi There"),
			 std::string("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
	CHECK_EQ(k4::HmacSha256Hex("Jefe", "what do ya want for nothing?"),
			 std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
	CHECK_EQ(k4::HmacSha256Hex(std::string(20, '\xaa'), std::string(50, '\xdd')),
			 std::string("773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"));
	CHECK_EQ(k4::HmacSha256Hex(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First"),
			 std::string("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));

	CHECK_EQ(k4::Crc32("123456789"), 0xCBF43926u);
	CHECK_EQ(k4::Crc32(""), 0u);
	CHECK_EQ(k4::Crc32("The quick brown fox jumps over the lazy dog"), 0x414FA339u);

	const k4::Sha256Digest digest = k4::Sha256("abc");
	CHECK_EQ(k4::HexEncode(digest.data(), digest.size()),
			 std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

// --- SigV4 -----------------------------------------------------------------------------------

struct AwsVector {
	const char* name;
	const char* method;
	const char* uri;
	const char* query;
	std::vector<std::pair<std::string, std::string>> headers;
	const char* payloadHash;
	const char* expectedSignature;
};

void TestSigV4() {
	const std::string emptyHash = k4::Sha256Hex("");
	const std::string putHash = k4::Sha256Hex("Welcome to Amazon S3.");
	CHECK_EQ(putHash, std::string("44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072"));

	const std::vector<AwsVector> vectors = {
		{"GET object",
		 "GET",
		 "/test.txt",
		 "",
		 {{"Host", "examplebucket.s3.amazonaws.com"},
		  {"Range", "bytes=0-9"},
		  {"x-amz-content-sha256", emptyHash},
		  {"x-amz-date", "20130524T000000Z"}},
		 emptyHash.c_str(),
		 "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41"},
		{"PUT object",
		 "PUT",
		 "/test%24file.text",
		 "",
		 {{"Date", "Fri, 24 May 2013 00:00:00 GMT"},
		  {"Host", "examplebucket.s3.amazonaws.com"},
		  {"x-amz-content-sha256", putHash},
		  {"x-amz-date", "20130524T000000Z"},
		  {"x-amz-storage-class", "REDUCED_REDUNDANCY"}},
		 putHash.c_str(),
		 "98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd"},
		{"GET lifecycle",
		 "GET",
		 "/",
		 "lifecycle=",
		 {{"Host", "examplebucket.s3.amazonaws.com"},
		  {"x-amz-content-sha256", emptyHash},
		  {"x-amz-date", "20130524T000000Z"}},
		 emptyHash.c_str(),
		 "fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543"},
		{"GET list objects",
		 "GET",
		 "/",
		 "max-keys=2&prefix=J",
		 {{"Host", "examplebucket.s3.amazonaws.com"},
		  {"x-amz-content-sha256", emptyHash},
		  {"x-amz-date", "20130524T000000Z"}},
		 emptyHash.c_str(),
		 "34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7"},
	};

	for (const AwsVector& vector : vectors) {
		k4::SigningInput input;
		input.method = vector.method;
		input.canonicalUri = vector.uri;
		input.canonicalQuery = vector.query;
		input.headers = vector.headers;
		input.payloadHash = vector.payloadHash;
		input.amzDate = "20130524T000000Z";
		input.region = "us-east-1";
		input.service = "s3";
		// The credentials printed on the AWS "Signature Calculation Examples" page (not the
		// AKIDEXAMPLE/+bPx... pair of the aws-sig-v4-test-suite, whose expected signatures differ).
		input.accessKey = "AKIAIOSFODNN7EXAMPLE";
		input.secretKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
		const k4::SigningResult result = k4::SignAwsV4(input);
		CheckString(result.signature, vector.expectedSignature, vector.name);
		if (std::string(vector.name) == "PUT object") {
			// The AWS documentation prints this canonical request verbatim as well.
			const std::string expectedCanonical =
				"PUT\n"
				"/test%24file.text\n"
				"\n"
				"date:Fri, 24 May 2013 00:00:00 GMT\n"
				"host:examplebucket.s3.amazonaws.com\n"
				"x-amz-content-sha256:44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072\n"
				"x-amz-date:20130524T000000Z\n"
				"x-amz-storage-class:REDUCED_REDUNDANCY\n"
				"\n"
				"date;host;x-amz-content-sha256;x-amz-date;x-amz-storage-class\n"
				"44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072";
			CheckString(result.canonicalRequest, expectedCanonical, "PUT canonical request");
		}
		if (std::string(vector.name) == "GET object") {
			// The AWS documentation prints this canonical request verbatim.
			const std::string expectedCanonical =
				"GET\n"
				"/test.txt\n"
				"\n"
				"host:examplebucket.s3.amazonaws.com\n"
				"range:bytes=0-9\n"
				"x-amz-content-sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n"
				"x-amz-date:20130524T000000Z\n"
				"\n"
				"host;range;x-amz-content-sha256;x-amz-date\n"
				"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
			CheckString(result.canonicalRequest, expectedCanonical, "GET canonical request");
			CheckString(result.signedHeaders, "host;range;x-amz-content-sha256;x-amz-date", "signed headers");
			CHECK(result.stringToSign.find("7344ae5b7ee6c3e7e6b0fe0640412a37625d1fbfff95c48bbb2dc43964946972")
				  != std::string::npos);
			CHECK(result.authorization.rfind("AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
											 "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, Signature=",
											 0)
				  == 0);
		}
	}

	CHECK_EQ(k4::AwsDateStamp("20130524T000000Z"), std::string("20130524"));
	CHECK_EQ(k4::FormatAmzDate(0), std::string("19700101T000000Z"));
	CHECK_EQ(k4::FormatAmzDate(1369353600), std::string("20130524T000000Z"));
}

void TestUriEncoding() {
	CHECK_EQ(k4::UriEncode("test$file.text", true), std::string("test%24file.text"));
	CHECK_EQ(k4::UriEncode("a b/c~d", true), std::string("a%20b%2Fc~d"));
	CHECK_EQ(k4::UriEncode("a b/c~d", false), std::string("a%20b/c~d"));
	CHECK_EQ(k4::UriEncode("AZaz09-._~", true), std::string("AZaz09-._~"));
	CHECK_EQ(k4::UriEncode("\xC3\xA9", true), std::string("%C3%A9"));
	CHECK_EQ(k4::UriEncode("a+b=c", true), std::string("a%2Bb%3Dc"));

	CHECK_EQ(k4::CanonicalQueryString({{"max-keys", "2"}, {"prefix", "J"}}), std::string("max-keys=2&prefix=J"));
	CHECK_EQ(k4::CanonicalQueryString({{"prefix", "a b/"}, {"list-type", "2"}}),
			 std::string("list-type=2&prefix=a%20b%2F"));
	CHECK_EQ(k4::CanonicalQueryString({{"uploads", ""}}), std::string("uploads="));
	CHECK_EQ(k4::CanonicalQueryString({{"b", "2"}, {"a", "z"}, {"a", "y"}}), std::string("a=y&a=z&b=2"));
}

// --- config ----------------------------------------------------------------------------------

void TestConfig() {
	k4::Config config = k4::DefaultConfig();
	std::string warning;
	CHECK(!k4::ParseConfig("", config, warning));
	CHECK_EQ(config.general.regularFileNamingPattern, std::string("{fileName}_{map}_{date}_{time}"));

	const std::string text = R"jsonc(
{
	// a comment
	"General": {
		"MinimumDemoDuration": 12.5,
		"DeleteDemoAfterUpload": false,
		"DemoDirectory": "my_demos",
		"AutoCleanupEnabled": true,
		"AutoCleanupIntervalMinutes": 15,
		"AutoCleanupFileAgeHours": 6
	},
	"AutoRecord": {
		"Enabled": false,
		"CropRounds": false,
		"StopOnIdle": true,
		"IdlePlayerCountThreshold": 3,
		"IdleTimeSeconds": 42
	},
	"DemoRequest": {
		"Enabled": true,
		"PrintAll": false,
		"DeleteUnused": false
	},
	"Discord": {
		"WebhookURL": "https://discord.com/api/webhooks/1/x",
		"ServerBoost": 3,
		"WebhookUploadFile": false
	},
	"S3": {
		"endpoint": "https://s3.example.com",
		"bucket": "demos",
		"access_key": "AK",
		"secret_key": "SK",
		"prefix": "demos/srv42",
		"path_style": true,
		"part_size_mb": 1,
		"retention_enabled": true,
		"retention_days": 0
	}
})jsonc";

	warning.clear();
	CHECK(k4::ParseConfig(text, config, warning));
	CHECK_EQ(config.general.minimumDemoDuration, 12.5);
	CHECK_EQ(config.general.deleteDemoAfterUpload, false);
	CHECK_EQ(config.general.demoDirectory, std::string("my_demos"));
	CHECK_EQ(config.general.autoCleanupIntervalMinutes, 15);
	CHECK_EQ(config.autoRecord.stopOnIdle, true);
	CHECK_EQ(config.autoRecord.idlePlayerCountThreshold, 3);
	CHECK_EQ(config.autoRecord.idleTimeSeconds, 42);
	CHECK_EQ(config.discord.serverBoost, 3);
	CHECK_EQ(config.discord.webhookUploadFile, false);
	CHECK_EQ(config.s3.region, std::string("us-east-1"));
	CHECK_EQ(config.s3.partSizeMb, 1); // NormalizeConfig (called by the plugin) clamps them below
	CHECK_EQ(config.s3.retentionDays, 0);

	k4::NormalizeConfig(config);
	// DemoRequest.Enabled forces the original's recording mode.
	CHECK_EQ(config.autoRecord.enabled, true);
	CHECK_EQ(config.autoRecord.cropRounds, true);
	CHECK_EQ(config.s3.partSizeMb, 5); // S3 minimum part size
	CHECK_EQ(config.s3.retentionDays, 1);
	CHECK_EQ(config.s3.prefix, std::string("demos/srv42/"));
	CHECK(config.s3Enabled());
	CHECK_EQ(config.maxDiscordFileSizeMb(), 100);

	// The original's config section wrapper is accepted too.
	k4::Config wrapped = k4::DefaultConfig();
	warning.clear();
	CHECK(k4::ParseConfig(R"({"K4GOTV":{"General":{"DemoDirectory":"wrapped"},"S3":{"bucket":"b"}}})", wrapped, warning));
	CHECK_EQ(wrapped.general.demoDirectory, std::string("wrapped"));
	CHECK_EQ(wrapped.s3.bucket, std::string("b"));

	// Wrong types keep the defaults and report a warning; the rest still applies.
	k4::Config wrong = k4::DefaultConfig();
	warning.clear();
	CHECK(k4::ParseConfig(R"({"General":{"MinimumDemoDuration":"nope","DemoDirectory":"ok"}})", wrong, warning));
	CHECK_EQ(wrong.general.minimumDemoDuration, 5.0);
	CHECK_EQ(wrong.general.demoDirectory, std::string("ok"));
	CHECK(!warning.empty());

	// Discord boost limits, original switch semantics.
	k4::Config boosted = k4::DefaultConfig();
	boosted.discord.serverBoost = 2;
	CHECK_EQ(boosted.maxDiscordFileSizeMb(), 50);
	boosted.discord.serverBoost = 1;
	CHECK_EQ(boosted.maxDiscordFileSizeMb(), 25);

	k4::Config empty = k4::DefaultConfig();
	CHECK(!empty.s3Enabled());
	empty.s3.endpoint = "s3.example.com";
	empty.s3.bucket = "b";
	empty.s3.accessKey = "a";
	CHECK(!empty.s3Enabled());
	empty.s3.secretKey = "s";
	CHECK(empty.s3Enabled());
}

// --- HTTP transport / signed request builder -------------------------------------------------

void TestHttpConfig() {
	k4::Config config = k4::DefaultConfig();
	CHECK_EQ(config.http.transport, std::string("auto"));
	CHECK(config.http.caFile.empty());

	std::string warning;
	CHECK(k4::ParseConfig(R"({"Http":{"transport":"CURL","ca_file":"/etc/ssl/custom.pem"}})", config, warning));
	CHECK(warning.empty());
	CHECK_EQ(config.http.transport, std::string("curl"));
	CHECK_EQ(config.http.caFile, std::string("/etc/ssl/custom.pem"));

	warning.clear();
	CHECK(k4::ParseConfig(R"({"Http":{"transport":"carrier-pigeon"}})", config, warning));
	CHECK_EQ(config.http.transport, std::string("auto"));
	CHECK(!warning.empty());

	// NormalizeConfig alone (the plugin calls it after every parse) also accepts a valid name.
	config.http.transport = "STEAM";
	k4::NormalizeConfig(config);
	CHECK_EQ(config.http.transport, std::string("steam"));

	// Surrounding whitespace around credentials/hostnames is trimmed (a trailing space in the
	// secret key otherwise makes every signature fail silently).
	k4::Config trimmed = k4::DefaultConfig();
	warning.clear();
	CHECK(k4::ParseConfig(R"({"S3":{"endpoint":" https://s3.example.com ","bucket":" demos ",
		"access_key":" AK ","secret_key":" SK ","region":" us-east-1 "},
		"Http":{"transport":" curl ","ca_file":" /etc/ssl/ca.pem "}})",
						  trimmed, warning));
	CHECK_EQ(trimmed.s3.endpoint, std::string("https://s3.example.com"));
	CHECK_EQ(trimmed.s3.bucket, std::string("demos"));
	CHECK_EQ(trimmed.s3.accessKey, std::string("AK"));
	CHECK_EQ(trimmed.s3.secretKey, std::string("SK"));
	CHECK_EQ(trimmed.s3.region, std::string("us-east-1"));
	CHECK_EQ(trimmed.http.transport, std::string("curl"));
	CHECK_EQ(trimmed.http.caFile, std::string("/etc/ssl/ca.pem"));
}

void TestS3SignedRequest() {
	k4::S3Settings settings;
	settings.endpoint = "https://firsts3.ru";
	settings.region = "us-east-1";
	settings.bucket = "cs2-gotv";
	settings.accessKey = "AKIAIOSFODNN7EXAMPLE";
	settings.secretKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
	settings.pathStyle = true;
	const std::time_t when = 1369353600; // 2013-05-24T00:00:00Z

	const std::string body = "hello";
	const k4::S3SignedRequest put = k4::BuildS3SignedRequest(settings, "firsts3.ru", "PUT",
															 "demos/test/a b.zip", false, {}, body,
															 "application/zip", {}, when);
	CHECK_EQ(put.url, std::string("https://firsts3.ru/cs2-gotv/demos/test/a%20b.zip"));
	CHECK_EQ(put.body, body);
	CHECK_EQ(put.contentType, std::string("application/zip"));
	CHECK_EQ(put.headers.size(), static_cast<std::size_t>(3));
	CHECK_EQ(put.headers[0].first, std::string("Authorization"));
	CHECK(put.headers[0].second.rfind("AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/"
									  "aws4_request, SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature=",
									  0)
		  == 0);
	CHECK_EQ(put.headers[1].second, k4::Sha256Hex(body));
	CHECK_EQ(put.headers[2].second, std::string("20130524T000000Z"));

	// A payload hash computed elsewhere (the uploader hashes each part on its file worker) must
	// yield exactly the same signed request as hashing the body here.
	const k4::S3SignedRequest prehashed =
		k4::BuildS3SignedRequest(settings, "firsts3.ru", "PUT", "demos/test/a b.zip", false, {}, body,
								 "application/zip", k4::Sha256Hex(body), {}, when);
	CHECK_EQ(prehashed.url, put.url);
	CHECK_EQ(prehashed.body, put.body);
	CHECK_EQ(prehashed.headers.size(), put.headers.size());
	for (std::size_t i = 0; i < put.headers.size() && i < prehashed.headers.size(); ++i) {
		CHECK_EQ(prehashed.headers[i].first, put.headers[i].first);
		CHECK_EQ(prehashed.headers[i].second, put.headers[i].second);
	}

	// Multipart Create: the uploads= query and the signed upload-identity header.
	const k4::S3SignedRequest create = k4::BuildS3SignedRequest(
		settings, "firsts3.ru", "POST", "demos/test/a.zip", false, {{"uploads", ""}}, {}, "",
		{{"x-amz-meta-k4-upload-id", "abc"}}, when);
	CHECK_EQ(create.url, std::string("https://firsts3.ru/cs2-gotv/demos/test/a.zip?uploads="));
	CHECK_EQ(create.headers.size(), static_cast<std::size_t>(4));
	CHECK(create.headers[0].second.find("SignedHeaders=host;x-amz-content-sha256;x-amz-date;"
										"x-amz-meta-k4-upload-id, Signature=")
		  != std::string::npos);
	CHECK_EQ(create.headers[1].second, k4::Sha256Hex(""));
	CHECK_EQ(create.headers[3].first, std::string("x-amz-meta-k4-upload-id"));
	CHECK_EQ(create.headers[3].second, std::string("abc"));

	// Bucket-level ListObjectsV2 (the retention path).
	const k4::S3SignedRequest list = k4::BuildS3SignedRequest(
		settings, "firsts3.ru", "GET", "", true, {{"list-type", "2"}, {"prefix", "demos/test-27035/"}}, {}, {},
		{}, when);
	CHECK_EQ(list.url, std::string("https://firsts3.ru/cs2-gotv?list-type=2&prefix=demos%2Ftest-27035%2F"));

	// Virtual-hosted style, like the AWS examples.
	k4::S3Settings virtualHosted = settings;
	virtualHosted.pathStyle = false;
	const k4::S3SignedRequest vh = k4::BuildS3SignedRequest(virtualHosted, "firsts3.ru", "HEAD", "a.zip", false, {},
															{}, {}, {}, when);
	CHECK_EQ(vh.url, std::string("https://cs2-gotv.firsts3.ru/a.zip"));
	CHECK(vh.headers[0].second.find("host;x-amz-content-sha256;x-amz-date") != std::string::npos);
}

void TestCurlTransport() {
	std::string version;
	if (!k4::CurlAvailable(&version)) {
		std::cout << "libcurl is not available; curl transport checks skipped\n";
		return;
	}
	CHECK(!version.empty());

	// Both rejections happen before any network access, so this is safe offline.
	k4::CurlRequest request;
	request.method = "PUT";
	request.url = "http://example.com/x";
	k4::CurlResponse rejected = k4::CurlPerform(request, "", nullptr);
	CHECK(rejected.transportFailed);
	CHECK(rejected.error.find("https") != std::string::npos);

	request.url = "https://example.com/x";
	request.method.clear();
	k4::CurlResponse noMethod = k4::CurlPerform(request, "", nullptr);
	CHECK(noMethod.transportFailed);
	CHECK(noMethod.error.find("method") != std::string::npos);
}

// --- naming / time ---------------------------------------------------------------------------

void TestShippedConfig() {
	// The package ships assets/config.jsonc; a syntax error there disables every setting at
	// runtime, so parse the real file here (the path is a compile definition from CMake).
	std::ifstream input(std::string(K4_GOTV_ASSET_DIR) + "/config.jsonc", std::ios::binary);
	CHECK(input.good());
	const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	CHECK(!text.empty());

	k4::Config config;
	std::string warning;
	CHECK(k4::ParseConfig(text, config, warning));
	CHECK(warning.empty());
	k4::NormalizeConfig(config);

	CHECK_EQ(config.general.demoDirectory, std::string("demos"));
	CHECK_EQ(config.general.regularFileNamingPattern, std::string("{fileName}_{map}_{date}_{time}"));
	CHECK_EQ(config.general.cropRoundsFileNamingPattern, std::string("{fileName}_{map}_round{round}_{date}_{time}"));
	CHECK_EQ(config.discord.webhookName, std::string("CSGO Demo Bot"));
	CHECK_EQ(config.s3.endpoint, std::string("https://firsts3.ru"));
	CHECK_EQ(config.s3.region, std::string("us-east-1"));
	CHECK_EQ(config.s3.bucket, std::string("cs2-gotv"));
	CHECK_EQ(config.s3.prefix, std::string("demos/<server>/"));
	CHECK_EQ(config.s3.pathStyle, true);
	CHECK_EQ(config.s3.partSizeMb, 16);
	CHECK_EQ(config.s3.retentionDays, 3);
	CHECK_EQ(config.http.transport, std::string("auto"));
	CHECK(config.http.caFile.empty());
	// The package must never contain credentials.
	CHECK(config.s3.accessKey.empty());
	CHECK(config.s3.secretKey.empty());
	CHECK(config.discord.webhookUrl.empty());
	CHECK(!config.s3Enabled()); // no keys -> uploads disabled until the operator fills them in
}

void TestNamingAndTime() {
	k4::NamingContext context;
	context.fileName = "autodemo";
	context.map = "de_dust2";
	context.date = "2026-10-18";
	context.time = "13-54-09";
	context.timestamp = "20261018_135409";
	context.round = 7;
	context.playerCount = 10;

	CHECK_EQ(k4::BuildDemoFileName("{fileName}_{map}_round{round}_{date}_{time}", context),
			 std::string("autodemo_de_dust2_round7_2026-10-18_13-54-09"));
	CHECK_EQ(k4::BuildDemoFileName("{timestamp}-{playerCount}", context), std::string("20261018_135409-10"));
	CHECK_EQ(k4::BuildDemoFileName("{unknown}_{map}", context), std::string("{unknown}_de_dust2"));
	CHECK_EQ(k4::AppendCollisionSuffix("autodemo", 1), std::string("autodemo_1"));
	CHECK_EQ(k4::AppendCollisionSuffix("autodemo", 23), std::string("autodemo_23"));
	// The recording loop accumulates: autodemo_1, autodemo_1_2, ... (the C# original).
	CHECK_EQ(k4::AppendCollisionSuffix(k4::AppendCollisionSuffix("autodemo", 1), 2),
			 std::string("autodemo_1_2"));

	CHECK_EQ(k4::LocalDate(0), std::string("1970-01-01"));
	CHECK_EQ(k4::LocalClock(0), std::string("00:00:00"));
	CHECK_EQ(k4::LocalFileTime(0), std::string("00-00-00"));
	CHECK_EQ(k4::LocalTimestamp(0), std::string("19700101_000000"));
	CHECK_EQ(k4::LocalDateTime(0), std::string("1970-01-01 00:00:00"));
	CHECK_EQ(k4::UtcIso8601(0), std::string("1970-01-01T00:00:00.0000000Z"));
	CHECK_EQ(k4::FormatDuration(307.9), std::string("05:07"));
	CHECK_EQ(k4::FormatDuration(5.0), std::string("00:05"));
	CHECK_EQ(k4::FormatDuration(0.0), std::string("00:00"));
	CHECK_EQ(k4::FormatDuration(-3.0), std::string("00:00"));
	// C# TimeSpan "mm\:ss": the minute component wraps every hour (the original's behaviour).
	CHECK_EQ(k4::FormatDuration(3600.0), std::string("00:00"));
	CHECK_EQ(k4::FormatDuration(3905.0), std::string("05:05"));
	CHECK_EQ(k4::ParseIso8601("2013-05-24T00:00:00.000Z"), static_cast<std::time_t>(1369353600));
	CHECK_EQ(k4::ParseIso8601("2013-05-24T00:00:00Z"), static_cast<std::time_t>(1369353600));
	CHECK_EQ(k4::ParseIso8601("not a date"), static_cast<std::time_t>(0));
}

// --- upload planning / retention -------------------------------------------------------------

void TestPlanningAndRetention() {
	constexpr uint64_t miB = 1024ull * 1024ull;

	k4::UploadPlan plan = k4::PlanUpload(0, 16 * miB);
	CHECK_EQ(plan.useMultipart, false);
	CHECK_EQ(plan.parts.size(), static_cast<std::size_t>(1));
	CHECK_EQ(plan.parts[0].length, static_cast<uint64_t>(0));

	plan = k4::PlanUpload(16 * miB, 16 * miB);
	CHECK_EQ(plan.useMultipart, false);
	CHECK_EQ(plan.parts.size(), static_cast<std::size_t>(1));
	CHECK_EQ(plan.parts[0].length, 16 * miB);

	plan = k4::PlanUpload(16 * miB + 1, 16 * miB);
	CHECK_EQ(plan.useMultipart, true);
	CHECK_EQ(plan.parts.size(), static_cast<std::size_t>(2));
	CHECK_EQ(plan.parts[0].offset, static_cast<uint64_t>(0));
	CHECK_EQ(plan.parts[0].length, 16 * miB);
	CHECK_EQ(plan.parts[1].offset, 16 * miB);
	CHECK_EQ(plan.parts[1].length, static_cast<uint64_t>(1));

	plan = k4::PlanUpload(100 * miB, 16 * miB);
	CHECK_EQ(plan.parts.size(), static_cast<std::size_t>(7));
	CHECK_EQ(plan.parts.back().length, 4 * miB);
	CHECK_EQ(plan.parts.back().offset, 96 * miB);

	// Requested parts below the S3 minimum are raised to 5 MiB.
	plan = k4::PlanUpload(6 * miB, 1 * miB);
	CHECK_EQ(plan.partSize, 5 * miB);
	CHECK_EQ(plan.parts.size(), static_cast<std::size_t>(2));

	// More than 10000 parts would be rejected by S3: the part size grows.
	const uint64_t large = 200ull * 1024ull * miB; // 200 GiB
	plan = k4::PlanUpload(large, 5 * miB);
	CHECK_EQ(plan.useMultipart, true);
	CHECK(plan.parts.size() <= 10000);
	CHECK_EQ(plan.partSize % miB, static_cast<uint64_t>(0));
	CHECK_EQ(plan.partSize, 21 * miB);
	CHECK_EQ(plan.parts.size(), static_cast<std::size_t>(9753));

	// Retention: the cut-off is inclusive, exactly like the original's >= RetentionHours.
	const std::time_t now = 1700000000;
	std::vector<k4::ObjectInfo> objects = {
		{"old", now - 3 * 86400 - 1},
		{"edge", now - 3 * 86400},
		{"fresh", now - 3 * 86400 + 1},
		{"unknown", 0},
	};
	std::vector<std::string> expired = k4::SelectExpiredObjects(objects, now, 3);
	CHECK_EQ(expired.size(), static_cast<std::size_t>(2));
	CHECK_EQ(expired[0], std::string("old"));
	CHECK_EQ(expired[1], std::string("edge"));

	// Retention days are clamped to at least one: an object one hour old survives even with 0.
	k4::ObjectInfo recent{"recent", now - 3600};
	CHECK_EQ(k4::SelectExpiredObjects({recent}, now, 0).size(), static_cast<std::size_t>(0));
}

// --- S3 keys / XML ---------------------------------------------------------------------------

void TestS3KeysAndXml() {
	CHECK_EQ(k4::NormalizeS3Prefix("demos/server"), std::string("demos/server/"));
	CHECK_EQ(k4::NormalizeS3Prefix("/demos/x/"), std::string("demos/x/"));
	CHECK_EQ(k4::NormalizeS3Prefix(""), std::string(""));
	CHECK_EQ(k4::NormalizeS3Prefix("demos/<server>/"), std::string("demos/<server>/"));
	CHECK_EQ(k4::S3ObjectKey("demos/srv/", "a dem.zip"), std::string("demos/srv/a dem.zip"));
	CHECK_EQ(k4::S3ObjectUrl("s3.example.com", "bucket", "demos/srv/a b.zip", true),
			 std::string("https://s3.example.com/bucket/demos/srv/a%20b.zip"));
	CHECK_EQ(k4::S3ObjectUrl("s3.example.com", "bucket", "demos/srv/a b.zip", false),
			 std::string("https://bucket.s3.example.com/demos/srv/a%20b.zip"));
	CHECK_EQ(k4::NormalizeEndpointHost("https://s3.example.com/"), std::string("s3.example.com"));
	CHECK_EQ(k4::NormalizeEndpointHost("s3.example.com"), std::string("s3.example.com"));
	CHECK_EQ(k4::NormalizeEndpointHost("s3.example.com:9000"), std::string("s3.example.com:9000"));
	// Default ports are dropped: the HTTP client omits them from Host, so signing them breaks SigV4.
	CHECK_EQ(k4::NormalizeEndpointHost("https://s3.example.com:443"), std::string("s3.example.com"));
	CHECK_EQ(k4::NormalizeEndpointHost("s3.example.com:443/"), std::string("s3.example.com"));
	CHECK_EQ(k4::NormalizeEndpointHost("https://s3.example.com:80"), std::string("s3.example.com"));
	CHECK_EQ(k4::NormalizeEndpointHost("[::1]:443"), std::string("[::1]"));
	CHECK_EQ(k4::NormalizeEndpointHost("[::1]:9000"), std::string("[::1]:9000"));
	CHECK_EQ(k4::NormalizeEndpointHost("s3.example.com/path"), std::string(""));
	CHECK_EQ(k4::NormalizeEndpointHost(""), std::string(""));
	CHECK(k4::EndpointIsHttps("s3.example.com"));
	CHECK(k4::EndpointIsHttps("https://s3.example.com"));
	CHECK(!k4::EndpointIsHttps("http://s3.example.com"));

	const std::string list = R"(<?xml version="1.0" encoding="UTF-8"?>
<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
	<Name>bucket</Name>
	<Prefix>demos/</Prefix>
	<KeyCount>2</KeyCount>
	<MaxKeys>1000</MaxKeys>
	<IsTruncated>true</IsTruncated>
	<NextContinuationToken>1ueGcxLPRx1Tr/XYExHnhbYLgveDs2J/wm36Hy4vbOwM=</NextContinuationToken>
	<Contents>
		<Key>demos/a&amp;b.zip</Key>
		<LastModified>2013-05-24T00:00:00.000Z</LastModified>
		<ETag>&quot;etag1&quot;</ETag>
		<Size>10</Size>
	</Contents>
	<Contents>
		<Key>demos/c.zip</Key>
		<LastModified>2026-10-18T12:00:00.000Z</LastModified>
		<Size>20</Size>
	</Contents>
</ListBucketResult>)";

	const k4::ListObjectsResult parsed = k4::ParseListObjectsV2(list);
	CHECK_EQ(parsed.objects.size(), static_cast<std::size_t>(2));
	CHECK_EQ(parsed.objects[0].key, std::string("demos/a&b.zip"));
	CHECK_EQ(parsed.objects[0].lastModified, static_cast<std::time_t>(1369353600));
	CHECK_EQ(parsed.objects[1].key, std::string("demos/c.zip"));
	CHECK_EQ(parsed.truncated, true);
	CHECK_EQ(parsed.nextContinuationToken, std::string("1ueGcxLPRx1Tr/XYExHnhbYLgveDs2J/wm36Hy4vbOwM="));
	CHECK(parsed.errorCode.empty());

	const std::string error = R"(<Error><Code>NoSuchKey</Code><Message>The specified key does not exist.</Message></Error>)";
	const k4::S3ErrorInfo info = k4::ParseS3Error(error);
	CHECK_EQ(info.code, std::string("NoSuchKey"));
	CHECK_EQ(info.message, std::string("The specified key does not exist."));
	CHECK(k4::IsS3ErrorDocument(error));
	CHECK(!k4::IsS3ErrorDocument(list));

	CHECK_EQ(k4::ParseUploadId("<InitiateMultipartUploadResult><Bucket>b</Bucket><Key>k</Key>"
							  "<UploadId>abc+def/ghi=</UploadId></InitiateMultipartUploadResult>"),
			 std::string("abc+def/ghi="));
	CHECK_EQ(k4::ParseXmlTag("<a><B>x</B></a>", "B"), std::string("x"));
	CHECK_EQ(k4::ParseXmlTag("<a><B><![CDATA[x<y]]></B></a>", "B"), std::string("x<y"));
	CHECK_EQ(k4::XmlUnescape("a&amp;b&lt;c&gt;d&quot;e&apos;f&#39;g&#x27;"), std::string("a&b<c>d\"e'f'g'"));

	CHECK_EQ(k4::BuildCompleteMultipartXml({{1, "\"etag1\""}, {2, "\"etag2\""}}),
			 std::string("<CompleteMultipartUpload><Part><PartNumber>1</PartNumber><ETag>\"etag1\"</ETag></Part>"
						 "<Part><PartNumber>2</PartNumber><ETag>\"etag2\"</ETag></Part></CompleteMultipartUpload>"));
}

// --- zip -------------------------------------------------------------------------------------

void TestZip() {
	const std::string source = "k4_gotv_zip_test_source.txt";
	const std::string target = "k4_gotv_zip_test.zip";
	const std::string content = "hello zip\n";
	std::error_code ignored;
	std::filesystem::remove(source, ignored);
	std::filesystem::remove(target, ignored);
	{
		std::ofstream out(source, std::ios::binary);
		out << content;
	}
	std::string error;
	CHECK(k4::WriteZipFile(source, target, "demo.dem", &error));
	CHECK(error.empty());

	std::ifstream in(target, std::ios::binary);
	CHECK(in.good());
	std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	CHECK(bytes.size() > 22 + content.size() + 30);
	CHECK_EQ(bytes.compare(0, 4, std::string("PK\x03\x04", 4)), 0);
	CHECK_EQ(bytes.compare(bytes.size() - 22, 4, std::string("PK\x05\x06", 4)), 0);
	CHECK(bytes.find("demo.dem") != std::string::npos);
	CHECK(bytes.find(content) != std::string::npos);

	// Local file header: CRC at 14, compressed size at 18, uncompressed at 22.
	auto read32 = [&bytes](std::size_t offset) {
		return static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset]))
			   | (static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8)
			   | (static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16)
			   | (static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
	};
	CHECK_EQ(read32(14), k4::Crc32(content));
	CHECK_EQ(read32(18), static_cast<uint32_t>(content.size()));
	CHECK_EQ(read32(22), static_cast<uint32_t>(content.size()));
	CHECK_EQ(static_cast<uint32_t>(static_cast<unsigned char>(bytes[8]))
				 | (static_cast<uint32_t>(static_cast<unsigned char>(bytes[9])) << 8),
			 0u); // method 0 = store

	// Entry name length at 26 and the name right after the 30-byte header.
	CHECK_EQ(read32(0), 0x04034b50u);
	CHECK_EQ(static_cast<uint32_t>(static_cast<unsigned char>(bytes[26]))
				 | (static_cast<uint32_t>(static_cast<unsigned char>(bytes[27])) << 8),
			 static_cast<uint32_t>(8));

	std::filesystem::remove(source, ignored);
	std::filesystem::remove(target, ignored);

	CHECK(!k4::WriteZipFile("does_not_exist_12345.dem", target, "x", &error));
	CHECK(!error.empty());
}

// --- Discord payload -------------------------------------------------------------------------

void TestDiscord() {
	k4::DiscordSettings settings;
	settings.webhookName = "CSGO Demo Bot";
	settings.webhookAvatar = "https://example.com/a.png";
	settings.messageText = "@everyone New CSGO Demo Available!";
	settings.embedTitle = "New CSGO Demo Available";

	k4::DiscordPayloadContext context;
	context.map = "de_dust2";
	context.date = "2026-10-18";
	context.time = "13:54:09";
	context.timedate = "2026-10-18 13:54:09";
	context.length = "05:07";
	context.round = 7;
	context.playerCount = 10;
	context.requesterNames = "Alpha, Beta";
	context.requesterSteamIds = "1, 2";
	context.requesterBoth = "Alpha (1)\nBeta (2)";
	context.requesterCount = 2;
	context.fileName = "autodemo_de_dust2";
	context.serverName = "My \"Server\"";
	context.isoTimestamp = "2026-10-18T13:54:09.000Z";
	context.downloadLinks = k4::DiscordDownloadLinks("https://s3.example.com/b/k.zip", "demos/k.zip");
	context.fileSizeBytes = 30ull * 1024ull * 1024ull;

	settings.serverBoost = 0;
	const std::string templateText = R"({"content":"{message_text}","links":"{download_links}","server":"{server_name}","size":"{fileSizeInKB}","warning":"{file_size_warning}","round":"{round}"})";
	const std::string payload = k4::BuildDiscordPayload(templateText, settings, context);
	CHECK(payload.find("\"content\":\"@everyone New CSGO Demo Available!\"") != std::string::npos);
	CHECK(payload.find("**S3:** [Click Here](https://s3.example.com/b/k.zip)\\n**Key:** `demos/k.zip`")
		  != std::string::npos);
	CHECK(payload.find("\"server\":\"My \\\"Server\\\"\"") != std::string::npos);
	CHECK(payload.find("\"size\":\"30720\"") != std::string::npos);
	CHECK(payload.find("\"warning\":\"\xE2\x9A\xA0\xEF\xB8\x8F File size (30MB) exceeds Discord limit.\"")
		  != std::string::npos);
	CHECK(payload.find("\"round\":\"7\"") != std::string::npos);

	settings.serverBoost = 3;
	const std::string noWarning = k4::BuildDiscordPayload(templateText, settings, context);
	CHECK(noWarning.find("\"warning\":\"\"") != std::string::npos);

	CHECK_EQ(k4::DiscordDownloadLinks("", "key"), std::string(""));
	CHECK_EQ(k4::JsonEscape("a\"b\\c\nd"), std::string("a\\\"b\\\\c\\nd"));

	const std::string fileData = "ZIPDATA";
	const std::string multipart = k4::BuildDiscordMultipart("BOUND", payload, "demo.zip", fileData);
	CHECK(multipart.rfind("--BOUND\r\n", 0) == 0);
	CHECK(multipart.find("Content-Disposition: form-data; name=\"payload_json\"\r\n"
						 "Content-Type: application/json\r\n\r\n")
		  != std::string::npos);
	CHECK(multipart.find(payload) != std::string::npos);
	CHECK(multipart.find("Content-Disposition: form-data; name=\"file\"; filename=\"demo.zip\"\r\n"
						 "Content-Type: application/zip\r\n\r\n")
		  != std::string::npos);
	CHECK(multipart.find(fileData) != std::string::npos);
	CHECK_EQ(multipart.compare(multipart.size() - 11, 9, std::string("--BOUND--")), 0);
	CHECK_EQ(k4::DiscordMultipartContentType("BOUND"), std::string("multipart/form-data; boundary=BOUND"));
}


// --- text helpers / KV1 / map gate / handle rule ----------------------------------------------

void TestTextHelpers() {
	CHECK_EQ(k4::Colorize("{GREEN}x"), std::string("\x04") + "x");
	CHECK_EQ(k4::Colorize("{LIGHTOLIVE}"), std::string("\x09"));
	CHECK_EQ(k4::Colorize("{OLIVE}"), std::string("\x10"));
	// The longer tag must be replaced before the shorter suffix could match.
	CHECK_EQ(k4::Colorize("{LIGHTPURPLE}"), std::string("\x03"));
	CHECK_EQ(k4::Colorize("{LIGHTRED}"), std::string("\x0F"));
	CHECK_EQ(k4::Colorize("a\\nb"), std::string("a\xe2\x80\xa9") + "b");
	CHECK_EQ(k4::Colorize("{GRAYBLUE}"), std::string("\x0A"));

	CHECK_EQ(k4::StripColorTags("{RED}warning{DEFAULT}"), std::string("warning"));
	CHECK_EQ(k4::StripColorTags("a\\nb"), std::string("a\nb"));
	CHECK_EQ(k4::StripColorTags("plain"), std::string("plain"));

	CHECK_EQ(k4::NormalizeLanguage("english"), std::string("en"));
	CHECK_EQ(k4::NormalizeLanguage("EN"), std::string("en"));
	CHECK_EQ(k4::NormalizeLanguage("portuguese"), std::string("pt-BR"));
	CHECK_EQ(k4::NormalizeLanguage("pt-br"), std::string("pt-BR"));
	CHECK_EQ(k4::NormalizeLanguage("brazilian"), std::string("pt-BR"));
	CHECK_EQ(k4::NormalizeLanguage("russian"), std::string("ru"));
	CHECK_EQ(k4::NormalizeLanguage("schinese"), std::string("zh-CN"));
	CHECK_EQ(k4::NormalizeLanguage("klingon"), std::string("klingon"));

	// A Discord webhook URL is a credential: only scheme + host may be logged.
	CHECK_EQ(k4::RedactUrl("https://discord.com/api/webhooks/123/token"), std::string("https://discord.com/..."));
	CHECK_EQ(k4::RedactUrl("https://firsts3.ru/bucket/key.zip"), std::string("https://firsts3.ru/..."));
	CHECK_EQ(k4::RedactUrl("https://discord.com"), std::string("https://discord.com"));
	CHECK_EQ(k4::RedactUrl("not a url"), std::string("..."));

	CHECK_EQ(k4::RedactSensitive("X-Amz-Signature=abcdef&X-Amz-Date=20260101T000000Z"),
			 std::string("X-Amz-Signature=***&X-Amz-Date=20260101T000000Z"));
	CHECK_EQ(k4::RedactSensitive("password=hunter2&user=cs2"), std::string("password=***&user=cs2"));
	CHECK_EQ(k4::RedactSensitive("secret_key=ABCDEF access_key=GHIJKL"),
			 std::string("secret_key=*** access_key=***"));
	// A Discord webhook URL is masked even without a query string (the token is the path).
	CHECK_EQ(k4::RedactSensitive("failed: https://discord.com/api/webhooks/123/token"),
			 std::string("failed: https://discord.com/..."));
	CHECK_EQ(k4::RedactSensitive("https://discord.com?token=abc https://firsts3.ru/x"),
			 std::string("https://discord.com?token=*** https://firsts3.ru/x"));
	// A plain S3 object URL (no query) is not altered by the defensive masker.
	CHECK_EQ(k4::RedactSensitive("https://firsts3.ru/bucket/demos/a.zip"),
			 std::string("https://firsts3.ru/bucket/demos/a.zip"));
}

void TestKv1() {
	k4::Kv1Node root;
	CHECK(k4::ParseKv1(R"(
// databases.cfg
"Databases"
{
	"k4_gotv"
	{
		"driver"   "sqlite"
		"host"     "127.0.0.1"
		"port"     "3307"
		"user"     cs2
	}
}
)", root));
	const k4::Kv1Node* section = root.Find("K4_GOTV"); // case-insensitive
	CHECK(section != nullptr);
	if (section != nullptr) {
		CHECK_EQ(section->GetString("driver", "mysql"), std::string("sqlite"));
		CHECK_EQ(section->GetString("host", ""), std::string("127.0.0.1"));
		CHECK_EQ(section->GetInt("port", 3306), 3307);
		CHECK_EQ(section->GetString("user", ""), std::string("cs2"));
		CHECK_EQ(section->GetString("missing", "fallback"), std::string("fallback"));
		CHECK_EQ(section->GetInt("missing", 42), 42);
		CHECK_EQ(section->GetInt("driver", 1), 1); // non-numeric -> fallback
	}

	// A hand-written single-section file is collapsed like Valve's loader does.
	k4::Kv1Node single;
	CHECK(k4::ParseKv1(R"("k4_gotv" { "host" "10.0.0.1" })", single));
	CHECK_EQ(single.GetString("host", ""), std::string("10.0.0.1"));

	k4::Kv1Node malformed;
	CHECK(!k4::ParseKv1(R"("Databases" { "k4_gotv" { "host" "x" )", malformed));
	CHECK(malformed.children.empty());
}

void TestMapGateAndHandleRule() {
	k4::MapGate gate;
	CHECK(!gate.Ready());
	CHECK(!gate.ListenersRegistered());
	// Without the four listeners the gate must never open, even on a live map.
	CHECK(!gate.OnPluginUpdate("de_dust2"));
	CHECK(!gate.Ready());

	gate.Reset();
	gate.SetListenersRegistered(true);
	// First update with no map: the one-shot latch refuses and is not retried.
	CHECK(!gate.OnPluginUpdate(""));
	CHECK(!gate.OnPluginUpdate("de_dust2"));
	CHECK(!gate.Ready());

	// A plugin loaded into a running map opens exactly on a real map name.
	gate.Reset();
	gate.SetListenersRegistered(true);
	CHECK(gate.OnPluginUpdate("de_dust2"));
	CHECK(gate.Ready());
	// "<empty>" is the engine's no-map sentinel.
	gate.Reset();
	gate.SetListenersRegistered(true);
	CHECK(!gate.OnPluginUpdate("<empty>"));

	// OnMapStart opens immediately; OnMapEnd closes and no later update may reopen.
	gate.Reset();
	gate.SetListenersRegistered(true);
	gate.OnMapStart();
	CHECK(gate.Ready());
	gate.OnMapEnd();
	CHECK(!gate.Ready());
	CHECK(!gate.OnPluginUpdate("de_dust2"));
	gate.OnMapStart();
	CHECK(gate.Ready());

	// OnPluginEnd closes the gate for good.
	gate.Close();
	CHECK(!gate.Ready());
	CHECK(!gate.OnPluginUpdate("de_dust2"));

	CHECK(!k4::IsValidEntityHandleValue(0));
	CHECK(!k4::IsValidEntityHandleValue(-1));
	CHECK(k4::IsValidEntityHandleValue(1));
	CHECK(k4::IsValidEntityHandleValue(-2));
	CHECK(k4::IsValidEntityHandleValue(0x7fffffff));
	CHECK(k4::IsValidEntityHandleValue(static_cast<std::int32_t>(0x80000000u)));
}

} // namespace

int RunS3StateTests();
int RunSqlTests();

int main() {
	// The time formatting helpers use localtime; pin the zone so the expected values are stable.
	setenv("TZ", "UTC", 1);
	tzset();

	TestCrypto();
	TestUriEncoding();
	TestSigV4();
	TestConfig();
	TestShippedConfig();
	TestHttpConfig();
	TestS3SignedRequest();
	TestCurlTransport();
	TestNamingAndTime();
	TestPlanningAndRetention();
	TestS3KeysAndXml();
	TestZip();
	TestDiscord();
	TestTextHelpers();
	TestKv1();
	TestMapGateAndHandleRule();
	g_failures += RunS3StateTests();
	g_failures += RunSqlTests();

	std::cout << g_checks << " checks, " << g_failures << " failure(s)\n";
	return g_failures == 0 ? 0 : 1;
}
