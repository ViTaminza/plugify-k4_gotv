# k4_gotv — Plugify port of K4-GOTV (SwiftlyS2 1.0.2)

Native C++ port of [K4ryuu/K4-GOTV-SwiftlyS2](https://github.com/K4ryuu/K4-GOTV-SwiftlyS2)
(`src/Plugin/*.cs`, `PluginConfig.cs`, `resources/`). Automatic GOTV recording, per-round crops,
the `!demo` request command, Discord webhook notifications, MySQL logging and local cleanup work
as in the original; FTP/SFTP/Mega.nz are replaced by an S3-compatible uploader (AWS Signature V4).

The plugin is **self-contained on s2sdk + plgdb** (docs/PLUGIN_STANDARD.md §7): no
`pisexcompat`, no `hudmenu` dependency and no other shared wrapper library. The safety rules the
wrapper used to enforce live in `src/`: the map gate (`src/map_gate.hpp`, PITFALLS B1/B2), the
client/controller/entity-handle checks (`src/ent_handle.hpp`, C1/C2), the map-gated warmup schema
read (C8), the update-driven timers (C9), and the symmetric unregistration of the map listeners,
game events and `demo` command (A7/C11/C13).

## Build and package

```bash
./build.sh plugify-k4_gotv k4_gotv_tests k4_gotv_http_tests k4_gotv_runtime_tests
./test.sh                                     # runs every build/tests/k4_gotv_* binary
bash plugins/k4_gotv/package.sh               # build/package/k4_gotv
```

A standalone configure of this directory also works (the exported repository layout): the CMake
file supplies the `s2sdk_bindings`, `plugify_sdk` and `libs/plgdb` fallbacks and can be configured
with `-DPLUGIFY_PORT_TESTS=OFF` for a release-only build.

Package layout:

```
k4_gotv.pplugin
bin/libk4_gotv.so
configs/k4_gotv/config.jsonc            (empty secrets, mode 600)
configs/k4_gotv/payload.json            (the original Discord template)
configs/k4_gotv/translations/en.jsonc
configs/k4_gotv/translations/pt-BR.jsonc
configs/k4_gotv/databases.cfg.example   (empty credentials, never a real databases.cfg)
```

**Deploy:** fill in `S3.access_key` / `S3.secret_key` and keep the file readable by the server user
only. `package.sh` installs it as mode 600; a `cp`/`rsync` without `-p` will not carry that over:

```bash
chmod 600 <plugify>/configs/k4_gotv/config.jsonc
```

The plugin never logs the keys (nor the Discord webhook URL).

## Configuration (`configs/k4_gotv/config.jsonc`)

The original's `k4-gotv.jsonc` layout minus FTP/SFTP/Mega, plus `S3`. A root `K4GOTV` object (the
original section) is accepted too. Comments (`//`) are allowed. Values are re-read when the file
changes (the original's `IOptionsMonitor` hot reload); S3 changes apply once no upload is pending.
Toggling `DemoRequest.Enabled`, `AutoCleanupEnabled`, `S3.retention_enabled` or
`AutoRecord.StopOnIdle` also starts or stops the update-driven idle check on reload.

| Section | Keys (defaults) |
| --- | --- |
| `General` | `MinimumDemoDuration` 5.0, `DeleteDemoAfterUpload` true, `DeleteZippedDemoAfterUpload` true, `DeleteEveryDemoFromServerAfterServerStart` false, `LogUploads` true, `LogDeletions` true, `RegularFileNamingPattern` `{fileName}_{map}_{date}_{time}`, `CropRoundsFileNamingPattern` `{fileName}_{map}_round{round}_{date}_{time}`, `DemoDirectory` `demos` (relative to the game dir), `AutoCleanupEnabled` false, `AutoCleanupIntervalMinutes` 60, `AutoCleanupFileAgeHours` 48 |
| `AutoRecord` | `Enabled` false, `CropRounds` false, `StopOnIdle` false, `RecordWarmup` true, `IdlePlayerCountThreshold` 0, `IdleTimeSeconds` 300 |
| `Discord` | `WebhookURL` "", `WebhookAvatar` "", `WebhookUploadFile` true, `WebhookName` "CSGO Demo Bot", `EmbedTitle` "New CSGO Demo Available", `MessageText` "@everyone New CSGO Demo Available!", `ServerBoost` 0 (0 -> 25 MB, 2 -> 50 MB, 3 -> 100 MB attachment limit) |
| `DemoRequest` | `Enabled` false, `PrintAll` true, `DeleteUnused` true. Enabling it forces `AutoRecord.Enabled` and `AutoRecord.CropRounds` on, like the original's config getter |
| `S3` | `endpoint` "https://firsts3.ru", `region` "us-east-1", `bucket` "cs2-gotv", `access_key` "", `secret_key` "", `prefix` "demos/<server>/", `path_style` true, `part_size_mb` 16, `retention_enabled` false, `retention_days` 3 |
| `Http` | `transport` "auto" (`auto` / `steam` / `curl`), `ca_file` "" (optional CA bundle for the libcurl transport) |

`prefix`: replace `<server>` with this server's identifier (a trailing `/` is added automatically).
The shipped endpoint/bucket target the lead's S3 (`https://firsts3.ru`, bucket `cs2-gotv`,
path-style, `us-east-1`).

The Discord embed and the database row carry the direct path-style object URL
(`https://<endpoint>/<bucket>/<key>`) plus the object key. The bucket `cs2-gotv` is private, so the
URL is downloadable only after a public-read policy (or a proxy/CDN) is configured; the key is
stored so a site or a signed-URL service can use it either way.

### Database (`configs/k4_gotv/databases.cfg`)

The metadata table uses **plgdb**; add a section named `k4_gotv` (Pisex KeyValues format, like the
other ported plugins):

```
"Databases"
{
    "k4_gotv"
    {
        "host"      "127.0.0.1"
        "port"      "3306"
        "user"      "cs2"
        "pass"      "..."
        "database"  "cs2"
    }
}
```

The package ships `databases.cfg.example` (empty credentials); copy it to `databases.cfg` and fill
in the connection of this server. The lookup order is the HEAD one: the Metamod spelling relative
to the server working directory first, then `<configs>/k4_gotv/databases.cfg`, the plugin's
translations directory and `<data>/k4_gotv/databases.cfg`. Without a section the plugin falls back
to a local SQLite file `<data dir>/k4_gotv/k4_gotv.sqlite3`. PostgreSQL is not supported by
plgdb. The table and columns
are the original's, unchanged: `k4-gotv` (`id`, `map`, `date`, `time`, `length`, `round`,
`mega_link`, `ftp_link`, `requester_name`, `requester_steamid`, `requester_count`, `player_count`,
`server_name`, `file_name`, index `idx_map_date`). The S3 object **URL** is written to `mega_link`
and the object **key** to `ftp_link` (the user's decision for the missing Mega/FTP services).

## Recording behaviour (original parity)

Trigger `round_start` (post; starts one frame later, like `NextWorldUpdate`) and `player_activate`
(post) start `tv_record "<game>/demos/<pattern>.dem"` when `AutoRecord.Enabled`, at least one real
player is connected (bots and the GOTV client excluded), `tv_enable` is 1, and (when
`RecordWarmup` is false) the game is not in warmup. Stop points: `cs_win_panel_match` (post),
map end, and — with `CropRounds` — the next `round_start`.

At stop the original waits 2 s for `tv_stoprecord` to finish writing, ignores demos shorter than
`MinimumDemoDuration`, deletes demos nobody requested when `DemoRequest.DeleteUnused` is on, then
zips `<demo>.dem` to `<demo>.zip`, uploads it, posts the Discord embed (with the zip attached when
it fits the server-boost limit), inserts the database row when an external upload succeeded, and
deletes the local files according to `DeleteDemoAfterUpload` / `DeleteZippedDemoAfterUpload`.
The webhook is best-effort: when it fails after a successful S3 upload the row is still written and
the cleanup still runs (see deviation 15).

`!demo` (also `/demo`) announces the request and adds the caller to the demo's requester list.
Without `CropRounds` the request list accumulates for the whole map, exactly like the original.
`StopOnIdle` starts the original's 1 s idle check (stop when the real player count stays below
`IdlePlayerCountThreshold` for `IdleTimeSeconds`). `AutoCleanupEnabled` deletes local `.dem`/`.zip`
files older than `AutoCleanupFileAgeHours` from the demo directory (mtime is used; .NET's
`GetCreationTime` is not available portably). `DeleteEveryDemoFromServerAfterServerStart` removes
every `.dem`/`.zip` of the demo directory once per plugin load: at the first map start after the
plugin loads, or at the config reload that enables it (not on every map change).

## S3 uploader

* AWS Signature V4 (HMAC-SHA256/SHA-256 implemented in `k4_crypto.cpp`, verified against the four
  official AWS S3 signature examples in the tests), HTTPS only, path-style by default.
* Files up to one part use `PutObject`; larger files use `CreateMultipartUpload` / `UploadPart` /
  `CompleteMultipartUpload` with `AbortMultipartUpload` on an unrecoverable failure. The effective
  part size is `part_size_mb`, capped at 64 MiB (both for the single-PUT threshold and for
  multipart parts, so no single buffer grows beyond that). Parts are read from disk one at a time
  on the worker thread: a multipart demo buffers at most one part, and the single-PUT path
  buffers only a file that is already no larger than the configured part size.
* If the `CompleteMultipartUpload` response is lost and the retry answers `NoSuchUpload`, the
  client HEADs the object: if S3 did commit it, the upload is reported as successful instead of
  failing a demo that actually exists.
* Transient failures (network, 408, 429, 5xx) are retried three times with 1 s / 2 s / 4 s
  back-off; other 4xx errors are final.
* Uploads start only after the original's 2 s `tv_stoprecord` delay and a file existence check.
* `retention_enabled`: every hour `ListObjectsV2` (paginated) under `prefix` is asked for the
  objects, and everything at least `retention_days` days old is removed with `DeleteObject`. This
  replaces the original's `uploads_retention.json` (FTP/Mega had no listing API; S3 has). One
  undeletable object (e.g. 403) is logged and the rest of the pass continues.
* A `CreateMultipartUpload` whose response is lost can leave an incomplete upload behind (its id
  is unknowable to the client). Configure an `AbortIncompleteMultipartUpload` lifecycle rule on
  the bucket if that matters.

## HTTP transport and the live Steam failure

Every request goes through `k4_http.cpp`. There are two transports:

* **libcurl** (`k4_curl.cpp`), the default: loaded with `dlopen` at runtime
  (`libcurl-gnutls.so.4`, `libcurl.so.4`, ...; no DT_NEEDED entry, no headers needed at build
  time), HTTP/1.1, no `Expect: 100-continue`, exact body, one request at a time on its own thread.
* **Steam** (`ISteamHTTP` from the server's `libsteam_api.so`, the original transport): used only
  when libcurl cannot be loaded, or when `Http.transport` is explicitly `"steam"`.

On the live 27035 server every S3 single PUT (7.7–11.7 MB) was reported as "network failure" ~10 s
after start: four Steam attempts (7 s of 1/2/4 s back-off) ending in a client-side failure with
**no HTTP response** (`m_bRequestSuccessful == false`, status 0). `rclone lsl` showed afterwards
that **all 24 objects were in the bucket with the exact sizes** — ISteamHTTP delivers the PUT and
then loses the response, so its "failure" is not a failed upload. Replaying such a request would
upload the object twice, so a request is attempted exactly once.

`Http.transport` selects the transport:

* `auto` (default) — **libcurl whenever it loads** on the host; ISteamHTTP only when libcurl is
  unavailable. Nothing depends on the Steam stack's broken response handling.
* `steam` — ISteamHTTP only, as before (diagnostic: objects may still arrive while the plugin
  reports failure).
* `curl` — libcurl only.

Endpoint normalization: an explicit default port in `S3.endpoint` (`:443` for https, `:80` for
http) is removed. HTTP clients omit it from the `Host` header, and signing it breaks the SigV4
canonical host (the lead's probe with an explicit `:443` got `AccessDenied`; without it, 200).

A failed transport is reported with the transport name and its detail (Steam `io failure`,
`request successful`, `status`; or `curl error N: ...`), and HTTP errors carry the S3 error XML.
If libcurl cannot be loaded on the host, the log says so (and the probe prints the exact signed
`curl` command instead of sending it). `Http.ca_file` points the curl transport at a specific CA
bundle if the runtime's store does not trust the endpoint's chain (firsts3.ru uses a GlobalSign
R46/AlphaSSL 2025 chain, which the Steam Runtime 3 images do trust).

### Host-side probe (`k4_gotv_s3probe`)

```bash
./build.sh k4_gotv_s3probe        # build/tools/k4_gotv_s3probe
AWS_ACCESS_KEY_ID=... AWS_SECRET_ACCESS_KEY=... build/tools/k4_gotv_s3probe
AWS_ACCESS_KEY_ID=... AWS_SECRET_ACCESS_KEY=... build/tools/k4_gotv_s3probe --multipart
AWS_ACCESS_KEY_ID=... AWS_SECRET_ACCESS_KEY=... build/tools/k4_gotv_s3probe --delete
```

The probe uses the same builder as the plugin (`k4::BuildS3SignedRequest`), so it sends the same
URL, signed headers and payload hash. It PUTs a 1 KB and a 10 MB object under `S3_PREFIX`
(default `demos/probe/`), GETs/HEADs them, and prints status, requested headers and the response
body on errors; `--multipart` reproduces the plugin's multipart path (default 11 MB in 5 MB
parts, `--part-size-mb`, `--size`); `--delete` removes the probe objects. Environment:
`AWS_ACCESS_KEY_ID`/`AWS_SECRET_ACCESS_KEY` (or `S3_ACCESS_KEY`/`S3_SECRET_KEY`), `S3_ENDPOINT`,
`S3_REGION`, `S3_BUCKET`, `S3_PREFIX`, `S3_PATH_STYLE`, `S3_CA_FILE`, `S3_TIMEOUT_SEC`. The
credentials are never printed or written to disk. If libcurl is unavailable it prints the exact
signed `curl` command (the signature, not the secret) for each request.

## Discord

The original's `payload.json` is shipped unchanged; every placeholder is replaced field for field
(`{map}`, `{length}`, `{round}`, `{player_count}`, `{date}`, `{time}`, `{fileName}`,
`{download_links}`, the requester fields, `{server_name}`, `{iso_timestamp}`,
`{file_size_warning}`, `{fileSizeInKB}`). The download link is `**S3:** [Click Here](url)` plus the
object key. Requests go through `k4_http.cpp`: libcurl by default (dlopen'ed, asynchronous, pumped
from `OnPluginUpdate`), ISteamHTTP from the server's `libsteam_api.so` only when libcurl is
unavailable — the Steam half is the transport of `plugins/cs2ac/src/webhook.cpp`, copied, not
linked.

## Tests

`tests/k4_gotv_tests.cpp` (engine-free, run by `./test.sh`): SHA-256 / HMAC-SHA256 (RFC 4231) /
CRC-32, four AWS SigV4 S3 vectors plus the exact canonical requests, URI/query canonicalisation,
config parsing/defaults/clamping (including the `Http` section), the shared signed-request builder
(plugin and probe), demo naming and collision suffixes, time formatting, multipart part planning
(5 MiB minimum, 10000-part limit), retention selection, ListObjectsV2 / multipart / error XML, the
ZIP container (header fields, CRC, EOCD), Discord payload escaping and the multipart body, the
dlopen'ed libcurl transport's local error paths, the colour/language/log-redaction helpers, the
KeyValues reader and every map-gate/handle-rule transition.

`tests/k4_s3_tests.cpp`: the real S3 state machine over an in-memory transport — multipart
ordering, lost-Complete HEAD verification, abort and its retries, the signed payload hash matching
the sent body, re-read after a failed part and exhausting the retry budget.

`tests/k4_sql_tests.cpp`: the `k4-gotv` table statements against a real in-memory SQLite database —
schema creation, placeholder/parameter match for both dialects, and the idempotent INSERT (a
retried statement does not duplicate the demo row). When `./test.sh` exports
`MYSQL_HOST/MYSQL_PORT/MYSQL_USER/MYSQL_PASSWORD/MYSQL_DATABASE`, the same statements also run
against the throwaway MariaDB; the test creates a random `k4_gotv_test_<pid>_<random>` table and
drops it again, so the production `k4-gotv` table is never touched.

`tests/k4_http_tests.cpp` (separate `k4_gotv_http_tests` binary): the real `HttpService` with no
Steam transport in the process, so requests run over libcurl — completion delivery on the game
thread, FIFO order, auto-mode routing and a prompt `Shutdown()` with a request in flight.

`tests/k4_gotv_runtime_tests.cpp` (separate `k4_gotv_runtime_tests` binary): the real
`src/plugin.cpp` compiled against recording `__s2sdk_*` fakes — no game server involved. It drives
the exported entry points and proves the B1/B2 gate (no client call before a map, the one-shot
mid-map latch with a real `GetCurrentMap()`, updates through a changelevel inert), the symmetric
A7 teardown and a clean restart, C9 (no s2sdk timer; recording starts/stops from
`OnPluginUpdate`), C11/C13 (server console -1, `GetEventPlayerSlot` for player fields, every hook
`Continue`), C1/C2 (0/-1 handles refused, a negative high-serial handle accepted) and the
`databases.cfg` path priority (a relative SQLite path lands under `<data>/k4_gotv`).

## Intentional deviations from the original

1. FTP/SFTP/Mega.nz are not ported; S3 replaces them (Mega/FTP config blocks are gone).
2. The S3 URL and key occupy the original's `mega_link` / `ftp_link` columns.
3. The zip is written with STORE (no compression library is linked): a valid `.zip`, but larger
   than the original's Deflate archive. Discord's size check therefore sees the stored size. The
   writer is classic ZIP without ZIP64, so a demo of 4 GiB or more cannot be zipped (the .NET
   original supported it); such a demo is left on disk and logged.
4. Retention uses `ListObjectsV2` instead of `uploads_retention.json`.
5. Discord values are JSON-escaped before insertion (the original inserted them verbatim, so a
   server name with a quote produced an invalid payload). Placeholders and values are unchanged.
6. A failed S3 upload keeps the local demo files (the original deleted them even when Mega reported
   an error text instead of a link).
7. `tv_enable` is checked before `tv_record` (the original relied on the README requirement).
8. Auto-cleanup compares last-write time (there is no portable creation time); the file currently
   being recorded is never deleted.
9. The original's `EmbedTitle` setting was already inert (its template carries a fixed title); the
   template is kept verbatim, so it stays inert.
10. If Discord (or the Steam HTTP layer) rejects a multipart request at the transport level or
    with HTTP 413, the embed is retried once without the zip attachment instead of failing the
    whole demo processing (the original simply threw).
11. The config file is `config.jsonc` (the task's name), not `k4-gotv.jsonc`; a `K4GOTV` root is
    still accepted. A config reload that cannot be read/parsed keeps the previous configuration
    (the original's `IOptionsMonitor` behaves the same for invalid files); only the first load
    falls back to the built-in defaults.
12. The original started a recording immediately only on a *hot reload* with players online; this
    port does the same on any load into a running map with players (the first update after the
    plugin starts), which is a superset of that behaviour.
13. `{iso_timestamp}` keeps .NET's seven fractional digits (`.0000000Z`).
14. A config edit during an upload/retention pass is applied as soon as that pass finishes (a
    second edit is not needed).
15. A Discord webhook failure no longer aborts the post-upload work: when the S3 upload succeeded
    the database row is still written and the local files are still cleaned up (the original
    skipped both, so a deleted/rotated webhook kept one demo per round on disk forever). When
    neither the S3 upload nor the webhook delivered the demo, the local files are kept.

## Live checks (the lead)

1. **First run the probe with the real keys** (see above). Exit 0 with two objects under `demos/probe/`
   proves URL, signature, TLS and the endpoint from the host. Any non-2xx prints the S3 error XML.
2. Start on the test server with `tv_enable 1`: the log shows `S3 uploads enabled: ...`, `libcurl
   fallback transport available (...)`, `loaded; ... HTTP transport auto`, and `Recording started: ...`
   on the first round.
3. After a round: the upload runs over libcurl (the default) and logs `Demo processed: ... ->
   https://...`. The zip appears in `<game>/demos` only until the cleanup; the S3 object is under
   `demos/<server>/`, the Discord embed carries the S3 link, the `k4-gotv` row contains URL and key.
   With `Http.transport: "steam"` ISteamHTTP delivers the object but the plugin still reports the
   lost response as a failure — do not use it outside diagnostics.
4. If the upload still fails, the log says which transport failed and why (`curl error N: ...`;
   Steam io failure/requestSuccessful/status only when libcurl is unavailable), and an HTTP error
   line carries the S3 error code/body. `Http.transport: "curl"` is equivalent to `auto` when
   libcurl loads; `"steam"` reproduces the old behaviour. For a Steam-only size test set
   `part_size_mb: 5` (multipart with 5 MiB parts).
5. Multipart: with `part_size_mb: 5`, record a > 5 MiB demo — check the object size and that no
   `UploadPart`/`Complete` errors appear.
6. `!demo` before a round end: chat message, demo kept, embed requested-by fields filled.
7. Retention: enable it with `retention_days: 1` and an old object under the prefix; the hourly
   pass (or a `plg reload` after the hour) deletes it.
8. `plg reload` with a demo recording: the plugin unloads cleanly (no crash), reloads and starts a
   new recording on the next round. The HTTP layer joins its curl thread during unload (an
   in-flight transfer is aborted through curl's progress callback).
