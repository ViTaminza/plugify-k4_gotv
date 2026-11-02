# k4_gotv (Plugify)

Port of the SwiftlyS2 plugin K4-GOTV (K4ryuu/K4-GOTV-SwiftlyS2 1.0.2): automatic GOTV recording (round-based crop, idle stop, warmup option, minimum duration), the !demo request command, Discord webhook notifications with the original embed and optional zip attachment, MySQL logging into the original `k4-gotv` table (SQLite fallback) and local demo cleanup. FTP/SFTP/Mega.nz are not ported: finished demos are uploaded to an S3-compatible endpoint (AWS Signature V4, single PUT or multipart, retention through ListObjectsV2/DeleteObject). Config: configs/k4_gotv/config.jsonc (secrets empty in the package; chmod 600 on deploy), payload/translations under configs/k4_gotv/.

Plugify port (fork) of **K4 GOTV** by K4ryuu.
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.2.0
- Plugify dependencies: s2sdk (>=3.0.0)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `k4_gotv-1.2.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/k4_gotv`.

**Testing status: NOT TESTED IN GAME. / Не тестировался в игре.** Standalone compilation and package checks do not validate gameplay, database, Discord or external service integrations.

## License
GPL-3.0, see LICENSE.

