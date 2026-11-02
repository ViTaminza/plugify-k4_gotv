// k4_gotv — self-contained SHA-256, HMAC-SHA256 and CRC-32 (see k4_crypto.hpp).
//
// Standard FIPS 180-4 SHA-256 and RFC 2104 HMAC. No external library: the plugin must load on a
// bare Steam Runtime 3 server without libcrypto or zlib.

#include "k4_crypto.hpp"

#include <cstring>

namespace k4 {
namespace {

constexpr uint32_t kSha256Initial[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
										0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

constexpr uint32_t kSha256Round[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline uint32_t RotateRight(uint32_t value, int bits) {
	return (value >> bits) | (value << (32 - bits));
}

void Sha256Compress(uint32_t state[8], const uint8_t block[64]) {
	uint32_t words[64];
	for (int index = 0; index < 16; ++index) {
		words[index] = (static_cast<uint32_t>(block[index * 4]) << 24) | (static_cast<uint32_t>(block[index * 4 + 1]) << 16)
					   | (static_cast<uint32_t>(block[index * 4 + 2]) << 8) | static_cast<uint32_t>(block[index * 4 + 3]);
	}
	for (int index = 16; index < 64; ++index) {
		const uint32_t s0 = RotateRight(words[index - 15], 7) ^ RotateRight(words[index - 15], 18) ^ (words[index - 15] >> 3);
		const uint32_t s1 = RotateRight(words[index - 2], 17) ^ RotateRight(words[index - 2], 19) ^ (words[index - 2] >> 10);
		words[index] = words[index - 16] + s0 + words[index - 7] + s1;
	}

	uint32_t a = state[0];
	uint32_t b = state[1];
	uint32_t c = state[2];
	uint32_t d = state[3];
	uint32_t e = state[4];
	uint32_t f = state[5];
	uint32_t g = state[6];
	uint32_t h = state[7];
	for (int index = 0; index < 64; ++index) {
		const uint32_t upper = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
		const uint32_t choice = (e & f) ^ (~e & g);
		const uint32_t temp1 = h + upper + choice + kSha256Round[index] + words[index];
		const uint32_t lower = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
		const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
		const uint32_t temp2 = lower + majority;
		h = g;
		g = f;
		f = e;
		e = d + temp1;
		d = c;
		c = b;
		b = a;
		a = temp1 + temp2;
	}
	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;
}

class Sha256Context {
public:
	Sha256Context() {
		std::memcpy(state_, kSha256Initial, sizeof(state_));
	}

	void Update(const uint8_t* data, std::size_t size) {
		total_ += size;
		while (size > 0) {
			const std::size_t space = sizeof(buffer_) - buffered_;
			const std::size_t take = size < space ? size : space;
			std::memcpy(buffer_ + buffered_, data, take);
			buffered_ += take;
			data += take;
			size -= take;
			if (buffered_ == sizeof(buffer_)) {
				Sha256Compress(state_, buffer_);
				buffered_ = 0;
			}
		}
	}

	Sha256Digest Final() {
		const uint64_t bitLength = total_ * 8u;
		const uint8_t one = 0x80;
		Update(&one, 1);
		const uint8_t zero = 0;
		while (buffered_ != 56) {
			Update(&zero, 1);
		}
		uint8_t lengthBytes[8];
		for (int index = 0; index < 8; ++index) {
			lengthBytes[index] = static_cast<uint8_t>(bitLength >> (56 - index * 8));
		}
		Update(lengthBytes, sizeof(lengthBytes));

		Sha256Digest digest {};
		for (int word = 0; word < 8; ++word) {
			digest[static_cast<std::size_t>(word) * 4] = static_cast<uint8_t>(state_[word] >> 24);
			digest[static_cast<std::size_t>(word) * 4 + 1] = static_cast<uint8_t>(state_[word] >> 16);
			digest[static_cast<std::size_t>(word) * 4 + 2] = static_cast<uint8_t>(state_[word] >> 8);
			digest[static_cast<std::size_t>(word) * 4 + 3] = static_cast<uint8_t>(state_[word]);
		}
		return digest;
	}

private:
	uint32_t state_[8] {};
	uint8_t buffer_[64] {};
	uint64_t total_ = 0; // bytes fed to Update, including padding bytes
	std::size_t buffered_ = 0;
};

// The padding bytes deliberately pass through Update() and land in total_; the encoded length must
// be the message length before padding. Sha256() records it first, like this wrapper does.
Sha256Digest Sha256Of(std::string_view data) {
	Sha256Context context;
	context.Update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
	return context.Final();
}

} // namespace

std::string HexEncode(const uint8_t* data, std::size_t size) {
	static constexpr char kDigits[] = "0123456789abcdef";
	std::string out;
	out.resize(size * 2);
	for (std::size_t index = 0; index < size; ++index) {
		out[index * 2] = kDigits[data[index] >> 4];
		out[index * 2 + 1] = kDigits[data[index] & 0x0f];
	}
	return out;
}

Sha256Digest Sha256(std::string_view data) {
	return Sha256Of(data);
}

std::string Sha256Hex(std::string_view data) {
	const Sha256Digest digest = Sha256Of(data);
	return HexEncode(digest.data(), digest.size());
}

Sha256Digest HmacSha256(std::string_view key, std::string_view data) {
	constexpr std::size_t kBlockSize = 64;
	uint8_t normalizedKey[kBlockSize] {};
	if (key.size() > kBlockSize) {
		const Sha256Digest hashed = Sha256Of(key);
		std::memcpy(normalizedKey, hashed.data(), hashed.size());
	} else if (!key.empty()) {
		std::memcpy(normalizedKey, key.data(), key.size());
	}

	uint8_t innerPad[kBlockSize];
	uint8_t outerPad[kBlockSize];
	for (std::size_t index = 0; index < kBlockSize; ++index) {
		innerPad[index] = static_cast<uint8_t>(normalizedKey[index] ^ 0x36u);
		outerPad[index] = static_cast<uint8_t>(normalizedKey[index] ^ 0x5cu);
	}

	Sha256Context inner;
	inner.Update(innerPad, kBlockSize);
	inner.Update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
	const Sha256Digest innerDigest = inner.Final();

	Sha256Context outer;
	outer.Update(outerPad, kBlockSize);
	outer.Update(innerDigest.data(), innerDigest.size());
	return outer.Final();
}

std::string HmacSha256Hex(std::string_view key, std::string_view data) {
	const Sha256Digest digest = HmacSha256(key, data);
	return HexEncode(digest.data(), digest.size());
}

namespace {

const std::array<uint32_t, 256>& Crc32Table() {
	static const auto table = [] {
		std::array<uint32_t, 256> values {};
		for (uint32_t index = 0; index < 256; ++index) {
			uint32_t value = index;
			for (int bit = 0; bit < 8; ++bit) {
				value = (value & 1u) != 0u ? (value >> 1) ^ 0xedb88320u : value >> 1;
			}
			values[index] = value;
		}
		return values;
	}();
	return table;
}

} // namespace

void Crc32Context::Update(const uint8_t* data, std::size_t size) {
	const std::array<uint32_t, 256>& table = Crc32Table();
	for (std::size_t index = 0; index < size; ++index) {
		m_state = table[(m_state ^ data[index]) & 0xffu] ^ (m_state >> 8);
	}
}

uint32_t Crc32Context::Value() const {
	return m_state ^ 0xffffffffu;
}

uint32_t Crc32(std::string_view data) {
	Crc32Context context;
	context.Update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
	return context.Value();
}

} // namespace k4
