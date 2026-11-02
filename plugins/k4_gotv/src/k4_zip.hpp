// k4_gotv — minimal ZIP writer.
//
// The original zipped the finished .dem with System.IO.Compression (Deflate) before uploading it
// to Mega/FTP and attaching it to Discord. This plugin links no compression library, so entries
// are written with the STORE method (method 0): a valid .zip that every unzipper reads, but the
// archive is not smaller than the demo. That is the one intentional deviation of the zip step;
// S3 stores the .zip (like the original uploaded the .zip) and Discord's size limit is checked
// against the stored size exactly like the original checked the deflated size.
//
// Single-entry archives only, which is all the original produced. The file is read twice (CRC
// pass, then data pass) so the local header carries the real CRC and sizes - no data descriptor.

#pragma once

#include <string>

namespace k4 {

// Zips `sourcePath` into `zipPath` with one entry named `entryName` (normally the .dem file's
// base name). Returns false and fills *error (when non-null) on any I/O failure; a partially
// written zip is removed.
bool WriteZipFile(const std::string& sourcePath, const std::string& zipPath, const std::string& entryName,
				  std::string* error = nullptr);

} // namespace k4
