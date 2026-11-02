// k4_gotv — minimal single-entry ZIP writer (STORE), see k4_zip.hpp.

#include "k4_zip.hpp"

#include "k4_crypto.hpp"

#include <array>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace k4 {
namespace {

constexpr uint32_t kLocalHeaderSignature = 0x04034b50u;
constexpr uint32_t kCentralHeaderSignature = 0x02014b50u;
constexpr uint32_t kEndOfCentralDirectorySignature = 0x06054b50u;
constexpr std::size_t kCopyBufferSize = 64 * 1024;
// Classic ZIP (no ZIP64) stores 32-bit sizes and offsets. Keep a margin for the headers so the
// central-directory offset cannot overflow.
constexpr uint64_t kMaximumSize = 0xffffffffull - 65536ull;

void WriteU16(std::ofstream& stream, uint16_t value) {
	const char bytes[2] = {static_cast<char>(value & 0xffu), static_cast<char>((value >> 8) & 0xffu)};
	stream.write(bytes, sizeof(bytes));
}

void WriteU32(std::ofstream& stream, uint32_t value) {
	const char bytes[4] = {static_cast<char>(value & 0xffu), static_cast<char>((value >> 8) & 0xffu),
						   static_cast<char>((value >> 16) & 0xffu), static_cast<char>((value >> 24) & 0xffu)};
	stream.write(bytes, sizeof(bytes));
}

void DosDateTime(uint16_t& dosTime, uint16_t& dosDate) {
	const std::time_t now = std::time(nullptr);
	std::tm brokenDown {};
	localtime_r(&now, &brokenDown);
	int year = brokenDown.tm_year + 1900;
	year = year < 1980 ? 1980 : (year > 2107 ? 2107 : year);
	dosTime = static_cast<uint16_t>((brokenDown.tm_hour << 11) | (brokenDown.tm_min << 5) | (brokenDown.tm_sec / 2));
	dosDate = static_cast<uint16_t>(((year - 1980) << 9) | ((brokenDown.tm_mon + 1) << 5) | brokenDown.tm_mday);
}

bool StreamFile(const std::string& path, Crc32Context* crc, std::ofstream* out, uint64_t& bytes) {
	std::ifstream input(path, std::ios::binary);
	if (!input) {
		return false;
	}
	std::array<char, kCopyBufferSize> buffer {};
	for (;;) {
		input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
		const std::streamsize got = input.gcount();
		if (got > 0) {
			if (crc != nullptr) {
				crc->Update(reinterpret_cast<const uint8_t*>(buffer.data()), static_cast<std::size_t>(got));
			}
			if (out != nullptr) {
				out->write(buffer.data(), got);
				if (!*out) {
					return false;
				}
			}
			bytes += static_cast<uint64_t>(got);
		}
		if (input.eof()) {
			return true;
		}
		if (!input) {
			return false;
		}
	}
}

} // namespace

bool WriteZipFile(const std::string& sourcePath, const std::string& zipPath, const std::string& entryName, std::string* error) {
	if (error != nullptr) {
		error->clear();
	}
	std::error_code filesystemError;
	const uint64_t sourceSize = std::filesystem::file_size(sourcePath, filesystemError);
	if (filesystemError) {
		if (error != nullptr) {
			*error = "cannot stat " + sourcePath + ": " + filesystemError.message();
		}
		return false;
	}
	if (sourceSize > kMaximumSize) {
		if (error != nullptr) {
			*error = "file is larger than the classic ZIP limit (4 GiB)";
		}
		return false;
	}
	if (entryName.size() > 0xffffu) {
		if (error != nullptr) {
			*error = "entry name is too long";
		}
		return false;
	}

	// Pass 1: CRC-32 without holding the demo in memory.
	Crc32Context crc;
	uint64_t crcBytes = 0;
	if (!StreamFile(sourcePath, &crc, nullptr, crcBytes) || crcBytes != sourceSize) {
		if (error != nullptr) {
			*error = "cannot read " + sourcePath;
		}
		return false;
	}
	const uint32_t crcValue = crc.Value();

	uint16_t dosTime = 0;
	uint16_t dosDate = 0;
	DosDateTime(dosTime, dosDate);

	bool utf8Name = false;
	for (const unsigned char character : entryName) {
		if (character >= 0x80) {
			utf8Name = true;
			break;
		}
	}
	const uint16_t flags = utf8Name ? 0x0800u : 0u; // bit 11: UTF-8 name

	std::ofstream out(zipPath, std::ios::binary | std::ios::trunc);
	if (!out) {
		if (error != nullptr) {
			*error = "cannot create " + zipPath;
		}
		return false;
	}

	const auto size32 = static_cast<uint32_t>(sourceSize);
	const auto nameLength = static_cast<uint16_t>(entryName.size());

	// Local file header.
	WriteU32(out, kLocalHeaderSignature);
	WriteU16(out, 20); // version needed
	WriteU16(out, flags);
	WriteU16(out, 0); // method 0 = store
	WriteU16(out, dosTime);
	WriteU16(out, dosDate);
	WriteU32(out, crcValue);
	WriteU32(out, size32);
	WriteU32(out, size32);
	WriteU16(out, nameLength);
	WriteU16(out, 0); // extra length
	out.write(entryName.data(), static_cast<std::streamsize>(entryName.size()));

	// Pass 2: the file data.
	uint64_t copied = 0;
	if (!out || !StreamFile(sourcePath, nullptr, &out, copied) || copied != sourceSize) {
		out.close();
		std::filesystem::remove(zipPath, filesystemError);
		if (error != nullptr) {
			*error = "cannot copy " + sourcePath + " into " + zipPath;
		}
		return false;
	}

	const auto centralOffset = static_cast<uint32_t>(30u + entryName.size() + sourceSize);

	// Central directory entry.
	WriteU32(out, kCentralHeaderSignature);
	WriteU16(out, 20); // version made by
	WriteU16(out, 20); // version needed
	WriteU16(out, flags);
	WriteU16(out, 0); // method
	WriteU16(out, dosTime);
	WriteU16(out, dosDate);
	WriteU32(out, crcValue);
	WriteU32(out, size32);
	WriteU32(out, size32);
	WriteU16(out, nameLength);
	WriteU16(out, 0); // extra
	WriteU16(out, 0); // comment
	WriteU16(out, 0); // disk number
	WriteU16(out, 0); // internal attributes
	WriteU32(out, 0); // external attributes
	WriteU32(out, 0); // local header offset
	out.write(entryName.data(), static_cast<std::streamsize>(entryName.size()));

	// End of central directory.
	const auto centralSize = static_cast<uint32_t>(46u + entryName.size());
	WriteU32(out, kEndOfCentralDirectorySignature);
	WriteU16(out, 0); // this disk
	WriteU16(out, 0); // disk with the central directory
	WriteU16(out, 1); // entries on this disk
	WriteU16(out, 1); // entries total
	WriteU32(out, centralSize);
	WriteU32(out, centralOffset);
	WriteU16(out, 0); // comment length
	out.flush();

	if (!out) {
		out.close();
		std::filesystem::remove(zipPath, filesystemError);
		if (error != nullptr) {
			*error = "cannot write " + zipPath;
		}
		return false;
	}
	return true;
}

} // namespace k4
