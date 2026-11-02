// k4_gotv — self-contained SHA-256 / HMAC-SHA256 / CRC32.
//
// The plugin links only the C++ runtime statically; it must not depend on libcrypto or zlib being
// present on the game server (Steam Runtime 3). AWS Signature V4 needs SHA-256 and HMAC-SHA256,
// the ZIP writer needs CRC-32. All of it is implemented here and covered by the AWS SigV4 test
// vectors in tests/k4_gotv_tests.cpp.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace k4 {

using Sha256Digest = std::array<uint8_t, 32>;

// Lowercase hex of `size` bytes.
std::string HexEncode(const uint8_t* data, std::size_t size);

Sha256Digest Sha256(std::string_view data);
std::string Sha256Hex(std::string_view data);

// RFC 2104 HMAC with SHA-256 (keys longer than the 64-byte block are hashed first).
Sha256Digest HmacSha256(std::string_view key, std::string_view data);
std::string HmacSha256Hex(std::string_view key, std::string_view data);

// zlib CRC-32 (polynomial 0xEDB88320). "123456789" -> 0xCBF43926.
uint32_t Crc32(std::string_view data);

// Incremental variant for the ZIP writer: Value() with no Update() is Crc32("").
class Crc32Context {
public:
	void Update(const uint8_t* data, std::size_t size);
	uint32_t Value() const;

private:
	uint32_t m_state = 0xffffffffu;
};

} // namespace k4
