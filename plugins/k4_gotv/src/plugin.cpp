// k4_gotv — Plugify C++ port of K4ryuu/K4-GOTV (SwiftlyS2 1.0.2, C#).
//
// Pure s2sdk + plgdb (docs/PLUGIN_STANDARD.md §7): no pisexcompat, no hudmenu dependency and no
// other shared wrapper library. Every safety rule the old wrapper enforced lives in this plugin:
//
//   * B1/B2 map gate (src/map_gate.hpp): the OnMapStart/OnMapEnd listeners are registered in
//     OnPluginStart and the gate opens on OnMapStart - or on the first OnPluginUpdate when the
//     plugin was loaded/reloaded into a running map, which never sees an OnMapStart. The mid-map
//     latch requires all four map-listener bindings, no OnMapEnd since OnPluginStart and a real
//     s2sdk::GetCurrentMap() value ("" / "<empty>" means no map). The gate drops on OnMapEnd and
//     closes on OnPluginEnd. Every client, entity and schema call asks Ready() first, so none can
//     reach the torn-down client list of a changelevel.
//   * A1/A7/C12: OnPluginStart never returns Failed. Every registration is checked against its
//     complete removal pair before anything is attached, and OnPluginEnd removes the map
//     listeners, the event hooks and the `demo` command symmetrically, then joins the file worker,
//     the HTTP service, the uploader, the Discord sender and the plgdb worker.
//   * C1/C2: slots are 0..63; IsClientConnected && IsClientInGame && !IsClientSourceTV; the
//     controller is PlayerSlotToEntPointer() validated with IsValidEntPointer() plus
//     PlayerSlotToEntHandle() validated with IsValidEntHandle(). 0/-1 are refused by the plugin
//     itself, while a negative handle (serial into bit 31) is valid (src/ent_handle.hpp).
//   * C8: the CCSGameRules::m_bWarmupPeriod offset is resolved only while the gate is open and
//     only a successful offset is cached; the raw byte read stays in the same tick as the rules
//     pointer (PITFALLS C6/C8).
//   * C9/B4: the plugin registers NO s2sdk timer. The original's 2 s post-record delay, the 1 s
//     idle check, the auto-cleanup interval and the hourly retention pass are accumulators driven
//     by OnPluginUpdate; deferred actions (the original's NextFrame) run on the next update. No
//     gameplay action ever runs inside s2sdk's TimerSystem callback lock.
//   * C11/C13: the `demo` command is a client-executable s2sdk console command (s2sdk also routes
//     its chat-triggered spelling to the same callback); server console (caller -1) and invalid
//     slots do nothing. Every event reads player fields with s2sdk::GetEventPlayerSlot and every
//     hook returns ResultType::Continue.
//
// Behaviour kept 1:1 with HEAD (the pisexcompat port), keys, defaults and side effects included;
// the deviations are the ones documented in README.md.

#include <plg/plugin.hpp>
#include <plg/source_location.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>

#include <s2sdk/s2sdk.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ent_handle.hpp"
#include "k4_core.hpp"
#include "k4_database.hpp"
#include "k4_discord.hpp"
#include "k4_discord_payload.hpp"
#include "k4_http.hpp"
#include "k4_s3.hpp"
#include "k4_text.hpp"
#include "k4_worker.hpp"
#include "k4_zip.hpp"
#include "map_gate.hpp"

namespace {

constexpr const char* kLogTag = "k4_gotv:";
constexpr const char* kPluginName = "k4_gotv";
constexpr const char* kDemoRequestCommand = "demo";
constexpr int kMaxSlots = 64;
constexpr double kProcessDelaySeconds = 2.0; // the original waits for tv_stoprecord to finish
constexpr double kIdleCheckSeconds = 1.0;
constexpr double kRetentionCheckSeconds = 3600.0;

k4::Config g_config;
std::string g_configPath;
std::filesystem::file_time_type g_configMtime {};
bool g_haveConfigMtime = false;

std::string g_demoDir;
std::string g_currentMap;
int g_roundNumber = 1;
int g_lastPlayerCount = 0;
double g_lastGameTime = 0.0;
std::string g_serverName = "Unknown Server";
bool g_tvDisabledWarned = false;
int g_warmupPeriodOffset = -1;

struct Requester {
	std::string name;
	uint64_t steamId = 0;
};

struct RecordingState {
	bool active = false;
	std::string fileName;
	std::string directory;
	double startTime = 0.0;
	double lastPlayerCheck = 0.0;
};

RecordingState g_recording;
std::vector<Requester> g_requesters;
bool g_requestedThisRound = false;

// Update-driven replacements for the original's timers (PITFALLS C9). All of them only run while
// the map gate is open; a time spent between maps is never accumulated, exactly like pisexcompat
// parked its timers during a changelevel.
bool g_idleChecksActive = false;
double g_idleAccumulator = 0.0;
double g_cleanupAccumulator = 0.0;
double g_retentionAccumulator = 0.0;
bool g_pendingIdleStop = false;
bool g_pendingRoundStartRecording = false;

k4::HttpService g_http;
k4::Worker g_worker;
k4::S3Client g_s3;
k4::DiscordService g_discord;
k4::DatabaseService g_database;

std::map<std::string, std::map<std::string, std::string>> g_translations;
std::string g_payloadTemplate;
bool g_payloadMissingLogged = false;

bool g_eventsHooked = false;
bool g_initialRecordingChecked = false;
bool g_s3ConfigPending = false;
bool g_startupCleanupDone = false;
bool g_pendingStartupCleanup = false;
bool g_demoCommandRegistered = false;
bool g_demoCommandOwned = false;
bool g_commandBindingsWarned = false;
bool g_unloading = false;
bool g_mapInitialized = false;

k4::MapGate g_mapGate;

// Defined later; the map listener and the configuration reload reference them.
void ResolveDemoDirectory();
void HandleMapReady();
void LoadPayloadTemplate();
void ApplyReloadableSettings();

// --- small helpers -----------------------------------------------------------------------------

std::string ToString(const plg::string& value) {
	return std::string(value.data(), value.size());
}

plg::string ToPlgString(const std::string& value) {
	return plg::string(value.data(), value.size());
}

std::string JoinPath(std::string_view dir, std::string_view leaf) {
	if (dir.empty()) {
		return std::string(leaf);
	}
	if (leaf.empty()) {
		return std::string(dir);
	}
	std::string path(dir);
	if (path.back() != '/' && path.back() != '\\') {
		path.push_back('/');
	}
	if (!leaf.empty() && leaf.front() == '/') {
		leaf.remove_prefix(1);
	}
	path.append(leaf);
	return path;
}

// <GetConfigsDir()>/<plugin>. With no plugify directory available this degrades to the relative
// "k4_gotv", exactly like pisexcompat's path helper did (HEAD's config path).
std::string ConfigDir() {
	const std::string root = plg::GetConfigsDir ? std::string(plg::GetConfigsDir()) : std::string();
	return JoinPath(root, kPluginName);
}

std::string ReadFile(std::string_view path, bool* ok) {
	std::string content;
	std::ifstream file(std::string(path), std::ios::binary);
	if (!file) {
		if (ok != nullptr) {
			*ok = false;
		}
		return content;
	}
	std::ostringstream buffer;
	buffer << file.rdbuf();
	content = buffer.str();
	if (ok != nullptr) {
		*ok = true;
	}
	return content;
}

void LogInfo(const std::string& message) {
	if (plg::Log) {
		plg::Log(std::string(kLogTag) + " " + k4::RedactSensitive(message), plg::Severity::Info,
				 plg::source_location::current());
	}
}

void LogWarning(const std::string& message) {
	if (plg::Log) {
		plg::Log(std::string(kLogTag) + " " + k4::RedactSensitive(message), plg::Severity::Warning,
				 plg::source_location::current());
	}
}

void LogError(const std::string& message) {
	if (plg::Log) {
		plg::Log(std::string(kLogTag) + " " + k4::RedactSensitive(message), plg::Severity::Error,
				 plg::source_location::current());
	}
}

std::string ReplaceAll(std::string text, std::string_view needle, std::string_view replacement) {
	if (needle.empty()) {
		return text;
	}
	std::string::size_type position = 0;
	while ((position = text.find(needle, position)) != std::string::npos) {
		text.replace(position, needle.size(), replacement);
		position += replacement.size();
	}
	return text;
}

std::string JoinNames(const std::vector<Requester>& requesters) {
	std::string joined;
	for (const Requester& requester : requesters) {
		if (!joined.empty()) {
			joined += ", ";
		}
		joined += requester.name;
	}
	return joined;
}

std::string JoinSteamIds(const std::vector<Requester>& requesters) {
	std::string joined;
	for (const Requester& requester : requesters) {
		if (!joined.empty()) {
			joined += ", ";
		}
		joined += std::to_string(requester.steamId);
	}
	return joined;
}

std::string JoinBoth(const std::vector<Requester>& requesters) {
	std::string joined;
	for (const Requester& requester : requesters) {
		if (!joined.empty()) {
			joined.push_back('\n');
		}
		joined += requester.name + " (" + std::to_string(requester.steamId) + ")";
	}
	return joined;
}

// --- client / entity gates (PITFALLS C1/C2) ----------------------------------------------------

bool SlotInRange(int slot) {
	return slot >= 0 && slot < kMaxSlots;
}

// The map gate plus the three s2sdk client tests. Every client-facing wrapper below starts here.
bool ClientInGame(int slot) {
	if (!g_mapGate.Ready() || !SlotInRange(slot)) {
		return false;
	}
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientInGame == nullptr
		|| __s2sdk_IsClientSourceTV == nullptr) {
		return false;
	}
	try {
		if (!s2sdk::IsClientConnected(slot)) {
			return false;
		}
		if (!s2sdk::IsClientInGame(slot)) {
			return false;
		}
		return !s2sdk::IsClientSourceTV(slot);
	} catch (...) {
		return false;
	}
}

// The slot also has a live controller entity. The pointer, its entity index and the handle are
// all validated; a negative handle is accepted (serial into bit 31), 0/-1 are not.
bool HasController(int slot) {
	if (!ClientInGame(slot) || __s2sdk_PlayerSlotToEntPointer == nullptr
		|| __s2sdk_PlayerSlotToEntHandle == nullptr || __s2sdk_IsValidEntPointer == nullptr
		|| __s2sdk_IsValidEntHandle == nullptr) {
		return false;
	}
	try {
		void* pointer = s2sdk::PlayerSlotToEntPointer(slot);
		if (pointer == nullptr || !s2sdk::IsValidEntPointer(pointer)) {
			return false;
		}
		const int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
		if (!k4::IsValidEntityHandleValue(handle)) {
			return false;
		}
		return s2sdk::IsValidEntHandle(handle);
	} catch (...) {
		return false;
	}
}

bool IsFakeClient(int slot) {
	if (!ClientInGame(slot) || __s2sdk_IsFakeClient == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsFakeClient(slot);
	} catch (...) {
		return true; // treat an unavailable client as not a real player
	}
}

bool IsRealPlayer(int slot) {
	return HasController(slot) && !IsFakeClient(slot);
}

int MaxPlayers() {
	if (!g_mapGate.Ready() || __s2sdk_GetMaxClients == nullptr) {
		return kMaxSlots;
	}
	try {
		const int maximum = s2sdk::GetMaxClients();
		return maximum > 0 && maximum <= kMaxSlots ? maximum : kMaxSlots;
	} catch (...) {
		return kMaxSlots;
	}
}

double GameTimeNow() {
	if (!g_mapGate.Ready() || __s2sdk_GetGameTime == nullptr) {
		return g_lastGameTime;
	}
	try {
		g_lastGameTime = static_cast<double>(s2sdk::GetGameTime());
	} catch (...) {
	}
	return g_lastGameTime;
}

void RefreshGameTime() {
	(void)GameTimeNow();
}

std::string CurrentMapName() {
	if (__s2sdk_GetCurrentMap == nullptr) {
		return {};
	}
	try {
		const std::string map = ToString(s2sdk::GetCurrentMap());
		return k4::IsUsableMapName(map) ? map : std::string();
	} catch (...) {
		return {};
	}
}

std::string GameDirectory() {
	if (!g_mapGate.Ready() || __s2sdk_GetGameDirectory == nullptr) {
		return {};
	}
	try {
		return ToString(s2sdk::GetGameDirectory());
	} catch (...) {
		return {};
	}
}

std::string PlayerName(int slot) {
	if (!HasController(slot) || __s2sdk_GetClientName == nullptr) {
		return {};
	}
	try {
		std::string name = ToString(s2sdk::GetClientName(slot));
		// s2sdk answers "<unknown>" when it cannot resolve the client; the Pisex API returned an
		// empty string there and callers treat "" as "no name".
		if (name == "<unknown>") {
			name.clear();
		}
		return name;
	} catch (...) {
		return {};
	}
}

uint64_t PlayerSteamId(int slot) {
	if (!ClientInGame(slot) || __s2sdk_GetClientSteamID64 == nullptr) {
		return 0;
	}
	try {
		return s2sdk::GetClientSteamID64(slot);
	} catch (...) {
		return 0;
	}
}

std::string PlayerLanguage(int slot) {
	if (!ClientInGame(slot) || __s2sdk_GetClientLanguage == nullptr) {
		return "en";
	}
	try {
		const std::string normalized = k4::NormalizeLanguage(ToString(s2sdk::GetClientLanguage(slot)));
		return normalized == "pt-BR" ? "pt-BR" : "en";
	} catch (...) {
		return "en";
	}
}

bool TvEnabled() {
	if (!g_mapGate.Ready() || __s2sdk_FindConVar == nullptr || __s2sdk_GetConVarBool == nullptr) {
		return true; // cannot check: let tv_record report the problem like the original
	}
	try {
		const uint64_t handle = s2sdk::FindConVar(plg::string("tv_enable"));
		if (handle == 0) {
			return true;
		}
		return s2sdk::GetConVarBool(handle);
	} catch (...) {
		return true;
	}
}

std::string ServerCvarString(const char* name) {
	if (!g_mapGate.Ready() || __s2sdk_FindConVar == nullptr || __s2sdk_GetConVarString == nullptr) {
		return {};
	}
	try {
		const uint64_t handle = s2sdk::FindConVar(plg::string(name));
		if (handle == 0) {
			return {};
		}
		return ToString(s2sdk::GetConVarString(handle));
	} catch (...) {
		return {};
	}
}

void RefreshServerName() {
	const std::string hostname = ServerCvarString("hostname");
	if (!hostname.empty()) {
		g_serverName = hostname;
	}
}

// The original's GetRealPlayerCount: valid, not a fake client, not HLTV.
int RealPlayerCount() {
	if (!g_mapGate.Ready()) {
		// Map shutdown: s2sdk's client list is torn down and any query dereferences null
		// (PITFALLS B2). Use the last value seen while the map was up.
		return g_lastPlayerCount;
	}
	int count = 0;
	const int maximum = MaxPlayers();
	for (int slot = 0; slot < maximum; ++slot) {
		if (!IsRealPlayer(slot)) {
			continue;
		}
		++count;
	}
	g_lastPlayerCount = count;
	return count;
}

int TotalRounds() {
	if (!g_mapGate.Ready() || __s2sdk_GetGameTotalRoundsPlayed == nullptr || __s2sdk_GetGameRules == nullptr) {
		return -1;
	}
	try {
		if (s2sdk::GetGameRules() == nullptr) {
			return -1;
		}
		return s2sdk::GetGameTotalRoundsPlayed();
	} catch (...) {
		return -1;
	}
}

void RefreshRoundNumber() {
	const int total = TotalRounds();
	if (total >= 0) {
		g_roundNumber = total + 1;
	}
}

bool WarmupPeriod() {
	if (!g_mapGate.Ready() || __s2sdk_GetGameRules == nullptr) {
		return false;
	}
	void* rules = nullptr;
	try {
		rules = s2sdk::GetGameRules();
	} catch (...) {
		return false;
	}
	if (rules == nullptr) {
		return false;
	}
	if (g_warmupPeriodOffset < 0 && __s2sdk_GetSchemaOffset != nullptr) {
		try {
			// C8: resolved only while a map is up; a failure is retried on the next call and only
			// a successful offset is cached.
			const int32_t offset = s2sdk::GetSchemaOffset(plg::string("CCSGameRules"), plg::string("m_bWarmupPeriod"));
			if (offset >= 0) {
				g_warmupPeriodOffset = offset;
			}
		} catch (...) {
			return false;
		}
	}
	if (g_warmupPeriodOffset < 0) {
		return false;
	}
	unsigned char value = 0;
	std::memcpy(&value, static_cast<const char*>(rules) + g_warmupPeriodOffset, sizeof(value));
	return value != 0;
}

// --- translations ------------------------------------------------------------------------------

// SwiftlyS2 colour tags ([lime], ...) -> the Pisex tags k4::Colorize understands.
std::string ConvertColorTags(std::string text) {
	static constexpr std::pair<const char*, const char*> kTags[] = {
		{"[default]", "{DEFAULT}"},		{"[white]", "{WHITE}"},			{"[darkred]", "{DARKRED}"},
		{"[red]", "{RED}"},				{"[lightred]", "{LIGHTRED}"},	{"[green]", "{GREEN}"},
		{"[lime]", "{LIME}"},			{"[lightgreen]", "{LIGHTGREEN}"},{"[darkblue]", "{BLUE}"},
		{"[blue]", "{BLUE}"},			{"[lightblue]", "{LIGHTBLUE}"},	{"[purple]", "{PURPLE}"},
		{"[lightpurple]", "{LIGHTPURPLE}"},{"[olive]", "{OLIVE}"},		{"[lightolive]", "{LIGHTOLIVE}"},
		{"[gray]", "{GRAY}"},			{"[grey]", "{GRAY}"},			{"[yellow]", "{OLIVE}"},
	};
	for (const auto& [from, to] : kTags) {
		text = ReplaceAll(std::move(text), from, to);
	}
	return text;
}

bool LoadTranslationFile(const std::string& path, std::map<std::string, std::string>& out) {
	bool ok = false;
	const std::string text = ReadFile(path, &ok);
	if (!ok) {
		return false;
	}
	try {
		const nlohmann::json root = nlohmann::json::parse(text.begin(), text.end(), nullptr,
														 /*allow_exceptions=*/true, /*ignore_comments=*/true);
		if (!root.is_object()) {
			return false;
		}
		for (auto iterator = root.begin(); iterator != root.end(); ++iterator) {
			if (iterator.value().is_string()) {
				out[iterator.key()] = iterator.value().get<std::string>();
			}
		}
		return true;
	} catch (const std::exception& exception) {
		LogError("cannot parse " + path + ": " + exception.what());
		return false;
	}
}

void LoadTranslations() {
	g_translations.clear();
	// HEAD's path: <GetConfigsDir()>/<plugin>/translations/<language>.jsonc. The two shipped
	// languages are the only ones the plugin uses.
	const std::string directory = JoinPath(ConfigDir(), "translations");
	for (const char* language : {"en", "pt-BR"}) {
		const std::string path = JoinPath(directory, std::string(language) + ".jsonc");
		if (!LoadTranslationFile(path, g_translations[language])) {
			LogWarning("translation file not found: " + path);
		}
	}
}

std::string Phrase(const std::string& language, const char* key) {
	auto languageIt = g_translations.find(language);
	if (languageIt != g_translations.end()) {
		auto keyIt = languageIt->second.find(key);
		if (keyIt != languageIt->second.end()) {
			return keyIt->second;
		}
	}
	auto english = g_translations.find("en");
	if (english != g_translations.end()) {
		auto keyIt = english->second.find(key);
		if (keyIt != english->second.end()) {
			return keyIt->second;
		}
	}
	return key; // visible, like Pisex's GetTranslation
}

void PrintLocalized(int slot, const char* key, const std::string& argument = {}) {
	if (!ClientInGame(slot) || __s2sdk_PrintToChat == nullptr) {
		return;
	}
	const std::string language = PlayerLanguage(slot);
	std::string message = Phrase(language, key);
	if (!argument.empty()) {
		message = ReplaceAll(std::move(message), "{0}", argument);
	}
	const std::string prefix = Phrase(language, "k4.general.prefix");
	const std::string line = " " + prefix + " " + message;
	try {
		s2sdk::PrintToChat(slot, ToPlgString(k4::Colorize(ConvertColorTags(line))));
	} catch (...) {
	}
}

// --- file helpers (worker thread only) ---------------------------------------------------------

// Deletes *.dem / *.zip files under `directory`; returns the paths that were removed. Never
// touches `skip` (the demo currently being recorded), and never touches a file written at or
// after `ignoreAtOrAfter` (the delete-all pass captures the request time so a recording that
// starts right after it - a plugin loaded into a running map runs both paths in one frame - is
// not destroyed by its own cleanup).
std::vector<std::string> DeleteDemoFiles(const std::string& directory, bool olderThanHoursSet, int olderThanHours,
										 const std::string& skip,
										 std::filesystem::file_time_type ignoreAtOrAfter = {}) {
	std::vector<std::string> deleted;
	std::error_code error;
	std::filesystem::directory_iterator iterator(directory, error);
	if (error) {
		return deleted;
	}
	const auto cutoff = std::filesystem::file_time_type::clock::now()
						- std::chrono::hours(std::max(1, olderThanHours));
	for (; iterator != std::filesystem::directory_iterator(); iterator.increment(error)) {
		if (error) {
			break;
		}
		const std::filesystem::directory_entry& entry = *iterator;
		std::error_code statusError;
		if (!entry.is_regular_file(statusError)) {
			continue;
		}
		const std::string path = entry.path().string();
		const std::string extension = entry.path().extension().string();
		if (extension != ".dem" && extension != ".zip") {
			continue;
		}
		if (!skip.empty() && path == skip) {
			continue;
		}
		if (ignoreAtOrAfter != std::filesystem::file_time_type {}) {
			std::error_code timeError;
			const auto modified = entry.last_write_time(timeError);
			if (timeError || modified >= ignoreAtOrAfter) {
				continue;
			}
		}
		if (olderThanHoursSet) {
			std::error_code timeError;
			const auto modified = entry.last_write_time(timeError);
			if (timeError || modified >= cutoff) {
				continue;
			}
		}
		std::error_code removeError;
		if (std::filesystem::remove(entry.path(), removeError) && !removeError) {
			deleted.push_back(path);
		}
	}
	return deleted;
}

bool DeleteOneFile(const std::string& path) {
	std::error_code error;
	const bool deleted = std::filesystem::remove(path, error);
	return deleted && !error;
}

void ReportDeletions(const std::vector<std::string>& deleted) {
	if (!g_config.general.logDeletions) {
		return;
	}
	for (const std::string& path : deleted) {
		LogInfo("Deleted: " + path);
	}
}

// The DeleteEveryDemoFromServerAfterServerStart pass: every .dem/.zip of the demo directory, once
// per plugin load. Runs at the first map start (the closest point to "server start"), or right
// then if the option is enabled by a config reload. `requestedAt` is captured before the job is
// posted, so a file written from that moment on (a recording started by the same update) survives.
void PostStartupCleanup() {
	if (g_startupCleanupDone) {
		return;
	}
	if (g_demoDir.empty()) {
		g_pendingStartupCleanup = true;
		return;
	}
	g_startupCleanupDone = true;
	const std::string directory = g_demoDir;
	const auto requestedAt = std::filesystem::file_time_type::clock::now();
	g_worker.Post([directory, requestedAt]() -> std::function<void()> {
		std::vector<std::string> deleted = DeleteDemoFiles(directory, false, 0, "", requestedAt);
		return [deleted = std::move(deleted)] { ReportDeletions(deleted); };
	});
}

// --- recording ---------------------------------------------------------------------------------

std::string ActiveDemoPath() {
	if (!g_recording.active) {
		return "";
	}
	return JoinPath(g_recording.directory, g_recording.fileName + ".dem");
}

void DisableIdleChecks() {
	g_idleChecksActive = false;
	g_idleAccumulator = 0.0;
}

void EnableIdleChecks() {
	g_idleChecksActive = true;
	g_idleAccumulator = 0.0;
}

void ResetRecordingState() {
	g_recording.active = false;
	g_recording.fileName.clear();
	g_recording.directory.clear();
	g_recording.startTime = 0.0;
	g_recording.lastPlayerCheck = 0.0;
	g_requestedThisRound = false;
}

// --- demo processing ---------------------------------------------------------------------------

struct DemoContext {
	std::string fileName;
	std::string demoPath;
	std::string zipPath;
	std::string map;
	std::string serverName;
	double duration = 0.0;
	int round = 1;
	int playerCount = 0;
	std::vector<Requester> requesters;
	bool wasRequested = false;
};

struct PendingDemoTask {
	std::chrono::steady_clock::time_point due;
	DemoContext context;
};

std::deque<PendingDemoTask> g_pendingDemos;

void FinishDemo(const DemoContext& context, bool uploadConfigured, const k4::UploadOutcome& outcome, bool discordOk,
				int discordStatus);

void OnUploadFinished(const DemoContext& context, uint64_t zipSize, bool uploadConfigured,
					  const k4::UploadOutcome& outcome) {
	if (uploadConfigured && !outcome.success) {
		LogError("S3 upload of '" + context.fileName + "' failed: " + outcome.error);
	}

	const std::time_t now = std::time(nullptr);
	k4::DiscordPayloadContext payloadContext;
	payloadContext.map = context.map;
	payloadContext.date = k4::LocalDate(now);
	payloadContext.time = k4::LocalClock(now);
	payloadContext.timedate = k4::LocalDateTime(now);
	payloadContext.length = k4::FormatDuration(context.duration);
	payloadContext.round = context.round;
	payloadContext.playerCount = context.playerCount;
	payloadContext.requesterNames = JoinNames(context.requesters);
	payloadContext.requesterSteamIds = JoinSteamIds(context.requesters);
	payloadContext.requesterBoth = JoinBoth(context.requesters);
	payloadContext.requesterCount = static_cast<int>(context.requesters.size());
	payloadContext.fileName = context.fileName;
	payloadContext.serverName = context.serverName;
	payloadContext.isoTimestamp = k4::UtcIso8601(now);
	payloadContext.downloadLinks =
		outcome.success ? k4::DiscordDownloadLinks(outcome.url, outcome.key) : std::string("No external uploads");
	payloadContext.fileSizeBytes = zipSize;

	const bool discordConfigured = g_discord.Enabled();
	const bool templateAvailable = !g_payloadTemplate.empty();
	if (discordConfigured && !templateAvailable && !g_payloadMissingLogged) {
		g_payloadMissingLogged = true;
		LogError("Discord payload template is missing (configs/k4_gotv/payload.json); the embed cannot be sent.");
	}

	if (discordConfigured && templateAvailable) {
		const uint64_t sizeMb = zipSize / (1024ull * 1024ull);
		const bool attach =
			g_config.discord.webhookUploadFile && sizeMb <= static_cast<uint64_t>(g_config.discord.maxDiscordFileSizeMb());
		const std::string payload = k4::BuildDiscordPayload(g_payloadTemplate, g_config.discord, payloadContext);
		g_discord.Send(payload, attach ? context.zipPath : std::string(), attach ? context.fileName + ".zip" : std::string(),
					   [context, uploadConfigured, outcome](bool ok, int status) {
						   FinishDemo(context, uploadConfigured, outcome, ok, status);
					   });
		return;
	}

	if (g_config.general.logUploads) {
		LogInfo("Demo processed (no Discord webhook): " + context.fileName);
	}
	FinishDemo(context, uploadConfigured, outcome, true, 0);
}

void FinishDemo(const DemoContext& context, bool uploadConfigured, const k4::UploadOutcome& outcome, bool discordOk,
				int discordStatus) {
	if (!discordOk) {
		// Best-effort webhook. The original's SendToDiscordAsync threw on a non-success status and
		// the catch in ProcessDemoAsync skipped the database row and the cleanup; on a busy server a
		// deleted/rotated webhook or a Discord outage would then keep one demo per round on disk
		// forever and drop its metadata row, although the S3 upload already succeeded.
		LogError("Demo processing failed: the Discord webhook request failed ("
				 + (discordStatus > 0 ? "HTTP " + std::to_string(discordStatus) : std::string("network failure")) + ").");
	}

	if (outcome.success) {
		const std::time_t now = std::time(nullptr);
		k4::DemoRecord record;
		record.map = context.map;
		record.date = k4::LocalDate(now);
		record.time = k4::LocalClock(now);
		record.length = k4::FormatDuration(context.duration);
		record.round = context.round;
		// The user's decision: the S3 URL and object key occupy the columns the original used for
		// the Mega and FTP links.
		record.megaLink = outcome.url;
		record.ftpLink = outcome.key;
		record.requesterName = JoinNames(context.requesters);
		record.requesterSteamId = JoinSteamIds(context.requesters);
		record.requesterCount = static_cast<int>(context.requesters.size());
		record.playerCount = context.playerCount;
		record.serverName = context.serverName;
		record.fileName = context.fileName;
		g_database.Store(record);
	}

	if (uploadConfigured && !outcome.success) {
		LogWarning("Local demo files kept because the S3 upload failed: " + context.demoPath);
		return;
	}
	if (!outcome.success && !discordOk) {
		// Neither the S3 upload nor the Discord webhook delivered the demo (S3 not configured):
		// the local files are the only copy left, so the cleanup options must not delete them.
		LogWarning("Local demo files kept because no upload delivered the demo: " + context.demoPath);
		return;
	}
	if (g_config.general.deleteDemoAfterUpload) {
		const std::string path = context.demoPath;
		g_worker.Post([path]() -> std::function<void()> {
			const bool deleted = DeleteOneFile(path);
			return [path, deleted] {
				if (deleted) {
					ReportDeletions({path});
				}
			};
		});
	}
	if (g_config.general.deleteZippedDemoAfterUpload) {
		const std::string path = context.zipPath;
		g_worker.Post([path]() -> std::function<void()> {
			const bool deleted = DeleteOneFile(path);
			return [path, deleted] {
				if (deleted) {
					ReportDeletions({path});
				}
			};
		});
	}
	if (g_config.general.logUploads) {
		if (outcome.success) {
			LogInfo("Demo processed: " + context.fileName + " -> " + outcome.url);
		} else {
			LogInfo("Demo processed: " + context.fileName);
		}
	}
	if (g_config.general.logUploads && g_discord.Enabled() && discordOk) {
		LogInfo("Demo uploaded to Discord: " + context.fileName);
	}
}

void ZipAndProcess(const DemoContext& context) {
	const std::string demoPath = context.demoPath;
	const std::string zipPath = context.zipPath;
	const std::string entryName = context.fileName + ".dem";
	g_worker.Post([demoPath, zipPath, entryName, context]() -> std::function<void()> {
		std::string error;
		const bool ok = k4::WriteZipFile(demoPath, zipPath, entryName, &error);
		return [ok, error, zipPath, context]() {
			if (!ok) {
				LogError("Zip failed for '" + context.fileName + "': " + error);
				return; // the original returns without touching the files
			}
			std::error_code filesystemError;
			const uint64_t zipSize = std::filesystem::file_size(zipPath, filesystemError);
			if (filesystemError) {
				LogError("Cannot stat the zipped demo " + zipPath + ": " + filesystemError.message());
				return;
			}
			LogInfo("Demo zipped: " + zipPath);
			if (g_s3.Configured()) {
				g_s3.UploadFile(zipPath, context.fileName + ".zip", "application/zip",
								[context, zipSize](k4::UploadOutcome outcome) {
									OnUploadFinished(context, zipSize, true, outcome);
								});
			} else {
				OnUploadFinished(context, zipSize, false, {});
			}
		};
	});
}

// The original waited 2 s for tv_stoprecord to finish writing. pisexcompat timers are gone
// (PITFALLS C9), so the delay is an update-driven accumulator and the file work runs from
// OnPluginUpdate, never inside an s2sdk timer callback.
void ScheduleDemoProcessing(const DemoContext& context) {
	PendingDemoTask task;
	task.due = std::chrono::steady_clock::now()
			   + std::chrono::milliseconds(static_cast<int64_t>(kProcessDelaySeconds * 1000.0));
	task.context = context;
	g_pendingDemos.push_back(std::move(task));
}

void ProcessDemoFile(const DemoContext& context) {
	std::error_code error;
	if (!std::filesystem::exists(context.demoPath, error)) {
		LogError("Demo file not found: " + context.demoPath);
		return;
	}
	if (g_config.demoRequest.enabled && !context.wasRequested) {
		if (g_config.demoRequest.deleteUnused) {
			const std::string path = context.demoPath;
			g_worker.Post([path]() -> std::function<void()> {
				const bool deleted = DeleteOneFile(path);
				return [path, deleted] {
					if (deleted) {
						ReportDeletions({path});
					}
				};
			});
		}
		return;
	}
	ZipAndProcess(context);
}

void ProcessPendingDemos() {
	if (!g_mapGate.Ready()) {
		return; // like pisexcompat: postponed until the next map
	}
	const auto now = std::chrono::steady_clock::now();
	while (!g_pendingDemos.empty() && now >= g_pendingDemos.front().due) {
		DemoContext context = std::move(g_pendingDemos.front().context);
		g_pendingDemos.pop_front();
		ProcessDemoFile(context);
	}
}

void StopRecording(bool scheduleProcessing = true) {
	DisableIdleChecks();
	if (!g_recording.active || g_recording.fileName.empty()) {
		ResetRecordingState();
		return;
	}

	if (__s2sdk_ServerCommand != nullptr) {
		try {
			s2sdk::ServerCommand(plg::string("tv_stoprecord"));
		} catch (...) {
		}
	}
	LogInfo("Recording stopped: " + g_recording.fileName);

	const double duration = GameTimeNow() - g_recording.startTime;
	if (duration < g_config.general.minimumDemoDuration) {
		ResetRecordingState();
		return;
	}

	DemoContext context;
	context.fileName = g_recording.fileName;
	context.demoPath = JoinPath(g_recording.directory, context.fileName + ".dem");
	context.zipPath = JoinPath(g_recording.directory, context.fileName + ".zip");
	context.map = g_currentMap;
	if (g_mapGate.Ready()) {
		RefreshServerName();
	}
	context.serverName = g_serverName;
	context.duration = duration;
	context.round = g_roundNumber;
	// The original reads TotalRoundsPlayed + 1 at stop time; do the same while the game rules are
	// reachable (they are not during a map change - then the cached value is used).
	const int liveTotal = TotalRounds();
	if (liveTotal >= 0) {
		context.round = liveTotal + 1;
	}
	// During a map change no client may be queried (PITFALLS B2): RealPlayerCount returns the
	// last value seen while the map was up.
	context.playerCount = RealPlayerCount();
	context.requesters = g_requesters;
	context.wasRequested = g_requestedThisRound;

	ResetRecordingState();
	if (scheduleProcessing && !g_unloading) {
		ScheduleDemoProcessing(context);
	}
}

void StartIdleTimer() {
	EnableIdleChecks();
}

void StartRecording(const std::string& baseName) {
	if (g_unloading || g_recording.active) {
		return;
	}
	if (!g_mapGate.Ready()) {
		// A deferred start must not run tv_record without a live map.
		return;
	}
	if (!TvEnabled()) {
		if (!g_tvDisabledWarned) {
			g_tvDisabledWarned = true;
			LogWarning("Automatic recording is waiting for tv_enable 1 (GOTV must be enabled).");
		}
		return;
	}
	if (!g_config.autoRecord.recordWarmup && WarmupPeriod()) {
		return;
	}

	RefreshRoundNumber();
	if (g_currentMap.empty()) {
		// A player/round event can arrive before the deferred map-start callback; the engine query
		// is safe here because a map is up and the gate is open.
		g_currentMap = CurrentMapName();
	}
	if (g_demoDir.empty()) {
		ResolveDemoDirectory();
	}
	const std::time_t now = std::time(nullptr);
	k4::NamingContext naming;
	naming.fileName = baseName;
	naming.map = g_currentMap;
	naming.date = k4::LocalDate(now);
	naming.time = k4::LocalFileTime(now);
	naming.timestamp = k4::LocalTimestamp(now);
	naming.round = g_roundNumber;
	naming.playerCount = RealPlayerCount();

	const std::string pattern = g_config.autoRecord.cropRounds ? g_config.general.cropRoundsFileNamingPattern
															  : g_config.general.regularFileNamingPattern;
	const std::string base = k4::BuildDemoFileName(pattern, naming);
	std::string fileName = base;
	std::error_code error;
	int suffix = 1;
	while (std::filesystem::exists(JoinPath(g_demoDir, fileName + ".dem"), error) && !error) {
		// The C# accumulates the suffix on the current name (name_1, name_1_2, ...).
		fileName = k4::AppendCollisionSuffix(fileName, suffix++);
		error.clear();
	}

	const std::string fullPath = JoinPath(g_demoDir, fileName + ".dem");
	if (__s2sdk_ServerCommand == nullptr) {
		LogError("Cannot start recording: the ServerCommand binding is unavailable.");
		return;
	}
	try {
		s2sdk::ServerCommand(ToPlgString("tv_record \"" + fullPath + "\""));
	} catch (const std::exception& exception) {
		LogError(std::string("tv_record failed: ") + exception.what());
		return;
	} catch (...) {
		LogError("tv_record failed");
		return;
	}

	g_recording.active = true;
	g_recording.fileName = fileName;
	g_recording.directory = g_demoDir;
	g_recording.startTime = GameTimeNow();
	g_recording.lastPlayerCheck = g_recording.startTime;
	LogInfo("Recording started: " + fileName);

	if (g_config.autoRecord.stopOnIdle) {
		StartIdleTimer();
	}
}

// --- update-driven timers (PITFALLS C9) --------------------------------------------------------

void UpdateIdleCheck(double dtSeconds) {
	if (!g_recording.active) {
		DisableIdleChecks();
		return;
	}
	if (!g_idleChecksActive) {
		return;
	}
	g_idleAccumulator += dtSeconds;
	if (g_idleAccumulator < kIdleCheckSeconds) {
		return;
	}
	g_idleAccumulator = 0.0;
	const int playerCount = RealPlayerCount();
	if (playerCount < g_config.autoRecord.idlePlayerCountThreshold) {
		const double idleTime = GameTimeNow() - g_recording.lastPlayerCheck;
		if (idleTime > g_config.autoRecord.idleTimeSeconds) {
			LogInfo("Stopping recording due to idle.");
			// Do not run the recording stop inside this update's timer-accumulator step? It is a
			// plain OnPluginUpdate callback (no s2sdk timer lock); the original deferred it to the
			// next frame through NextFrame, which ProcessDeferred() below reproduces.
			DisableIdleChecks();
			g_pendingIdleStop = true;
		}
	} else {
		g_recording.lastPlayerCheck = GameTimeNow();
	}
}

void UpdateAutoCleanup(double dtSeconds) {
	if (!g_config.general.autoCleanupEnabled) {
		g_cleanupAccumulator = 0.0;
		return;
	}
	const double interval = static_cast<double>(std::max(1, g_config.general.autoCleanupIntervalMinutes)) * 60.0;
	g_cleanupAccumulator += dtSeconds;
	if (g_cleanupAccumulator < interval) {
		return;
	}
	g_cleanupAccumulator = 0.0;
	const std::string directory = g_demoDir;
	if (directory.empty()) {
		return;
	}
	const int hours = std::max(1, g_config.general.autoCleanupFileAgeHours);
	const std::string active = ActiveDemoPath();
	g_worker.Post([directory, hours, active]() -> std::function<void()> {
		std::vector<std::string> deleted = DeleteDemoFiles(directory, true, hours, active);
		return [deleted = std::move(deleted)] { ReportDeletions(deleted); };
	});
}

void UpdateRetention(double dtSeconds) {
	if (!g_config.s3.retentionEnabled || !g_s3.Configured()) {
		g_retentionAccumulator = 0.0;
		return;
	}
	g_retentionAccumulator += dtSeconds;
	if (g_retentionAccumulator < kRetentionCheckSeconds) {
		return;
	}
	g_retentionAccumulator = 0.0;
	const int days = std::max(1, g_config.s3.retentionDays);
	g_s3.RetentionScan(std::time(nullptr), days, [](int deleted, std::string error) {
		if (!error.empty()) {
			LogError("S3 retention failed: " + error);
		} else if (deleted > 0) {
			LogInfo("S3 retention deleted " + std::to_string(deleted) + " object(s).");
		}
	});
}

void ProcessDeferredActions() {
	if (g_pendingIdleStop) {
		g_pendingIdleStop = false;
		StopRecording();
	}
	if (g_pendingRoundStartRecording) {
		g_pendingRoundStartRecording = false;
		if (!g_recording.active && g_config.autoRecord.enabled) {
			StartRecording("autodemo");
		}
	}
}

// --- events ------------------------------------------------------------------------------------

void HandleRoundStart() {
	if (!g_mapGate.Ready() || !g_config.autoRecord.enabled) {
		return;
	}
	RefreshRoundNumber();
	if (g_config.autoRecord.cropRounds && g_recording.active) {
		StopRecording();
	}
	if (g_config.autoRecord.cropRounds) {
		g_requesters.clear();
	}
	if (!g_recording.active) {
		const int playerCount = RealPlayerCount();
		if (playerCount > 0) {
			// The original deferred the start to the next world update.
			g_pendingRoundStartRecording = true;
		}
	}
}

void HandlePlayerActivate(void* event) {
	if (!g_mapGate.Ready() || event == nullptr || __s2sdk_GetEventPlayerSlot == nullptr) {
		return;
	}
	const int slot = static_cast<int>(s2sdk::GetEventPlayerSlot(event, plg::string("userid")));
	if (!IsRealPlayer(slot)) {
		return;
	}
	g_recording.lastPlayerCheck = GameTimeNow();
	if (!g_recording.active && g_config.autoRecord.enabled) {
		StartRecording("autodemo");
	}
}

s2sdk::ResultType OnRoundStartEvent(const plg::string& /*name*/, void* /*event*/, bool /*dontBroadcast*/) {
	try {
		HandleRoundStart();
	} catch (...) {
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnPlayerActivateEvent(const plg::string& /*name*/, void* event, bool /*dontBroadcast*/) {
	try {
		HandlePlayerActivate(event);
	} catch (...) {
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnMatchEndEvent(const plg::string& /*name*/, void* /*event*/, bool /*dontBroadcast*/) {
	try {
		StopRecording();
	} catch (...) {
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnDemoCommand(int32_t caller, s2sdk::ConCommandContext context,
								const plg::vector<plg::string>& /*args*/) {
	try {
		if (g_unloading) {
			return s2sdk::ResultType::Handled;
		}
		// A server-console caller is exactly -1. Any other invalid caller (a negative value or an
		// out-of-range slot) is not a player and gets no chat line either.
		if (caller != -1 && !IsRealPlayer(caller)) {
			return s2sdk::ResultType::Handled;
		}
		if (caller == -1) {
			return s2sdk::ResultType::Continue; // client command: the server console does nothing
		}
		const std::string name = PlayerName(caller);
		if (g_config.demoRequest.printAll && !g_requestedThisRound) {
			const int maximum = MaxPlayers();
			for (int target = 0; target < maximum; ++target) {
				if (!IsRealPlayer(target) || IsFakeClient(target)) {
					continue;
				}
				PrintLocalized(target, "k4.chat.demo.request.all", name);
			}
		} else {
			PrintLocalized(caller, "k4.chat.demo.request.self");
		}

		const uint64_t steamId = PlayerSteamId(caller);
		const bool already = std::any_of(g_requesters.begin(), g_requesters.end(),
										 [steamId](const Requester& requester) { return requester.steamId == steamId; });
		if (!already) {
			g_requesters.push_back({name.empty() ? std::string("Unknown") : name, steamId});
		}
		g_requestedThisRound = true;
		(void)context;
		return s2sdk::ResultType::Handled;
	} catch (...) {
		return s2sdk::ResultType::Handled;
	}
}

void UnregisterDemoCommand() {
	if (!g_demoCommandRegistered) {
		return;
	}
	g_demoCommandRegistered = false;
	const bool owned = g_demoCommandOwned;
	g_demoCommandOwned = false;
	try {
		if (owned) {
			if (__s2sdk_RemoveCommand != nullptr) {
				s2sdk::RemoveCommand(plg::string(kDemoRequestCommand), &OnDemoCommand);
			}
		} else if (__s2sdk_RemoveCommandListener != nullptr) {
			// The name already existed: only our listener may be removed; RemoveCommand would
			// unregister callbacks that are not ours.
			s2sdk::RemoveCommandListener(plg::string(kDemoRequestCommand), &OnDemoCommand, s2sdk::HookMode::Pre);
		}
	} catch (...) {
	}
}

void RegisterDemoCommand() {
	if (g_demoCommandRegistered) {
		return;
	}
	// The complete registration/removal pair is required before anything is attached (A1/A7):
	// AddConsoleCommand tells us whether the name is ours, RemoveCommand removes an owned command
	// and RemoveCommandListener detaches the listener of a pre-existing one.
	if (__s2sdk_AddConsoleCommand == nullptr || __s2sdk_RemoveCommand == nullptr
		|| __s2sdk_RemoveCommandListener == nullptr) {
		if (!g_commandBindingsWarned) {
			g_commandBindingsWarned = true;
			LogError("The s2sdk console-command bindings are unavailable; the !demo command is disabled.");
		}
		return;
	}
	try {
		const auto flags = static_cast<s2sdk::ConVarFlag>(static_cast<int64_t>(s2sdk::ConVarFlag::Release)
														  | static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute));
		g_demoCommandOwned = s2sdk::AddConsoleCommand(plg::string(kDemoRequestCommand),
													  plg::string("k4_gotv: request the demo upload"), flags,
													  &OnDemoCommand, s2sdk::HookMode::Pre);
		g_demoCommandRegistered = true;
	} catch (const std::exception& exception) {
		LogError(std::string("registering the demo command failed: ") + exception.what());
	} catch (...) {
		LogError("registering the demo command failed");
	}
}

void ApplyDemoCommandSetting() {
	if (g_config.demoRequest.enabled) {
		if (!g_demoCommandRegistered) {
			RegisterDemoCommand();
			if (g_demoCommandRegistered) {
				LogInfo("The !demo request command is enabled.");
			}
		}
		return;
	}
	if (g_demoCommandRegistered) {
		UnregisterDemoCommand();
		LogInfo("The !demo request command is disabled (DemoRequest.Enabled = false).");
	}
}

// --- hooks / lifecycle -------------------------------------------------------------------------

struct EventHook {
	const char* name;
	s2sdk::EventCallback callback;
	bool hooked;
};

EventHook kEventHooks[] = {
	{"round_start", &OnRoundStartEvent, false},
	{"player_activate", &OnPlayerActivateEvent, false},
	{"cs_win_panel_match", &OnMatchEndEvent, false},
};

void HookEvents() {
	if (g_eventsHooked) {
		return;
	}
	// Hook and unhook are only useful as a pair: a hook that cannot be removed would point into
	// this library after unload (PITFALLS A7).
	if (__s2sdk_HookEvent == nullptr || __s2sdk_UnhookEvent == nullptr) {
		LogError("The s2sdk event-hook pair is unavailable; no game event is hooked.");
		return;
	}
	bool any = false;
	for (EventHook& hook : kEventHooks) {
		try {
			const s2sdk::EventHookError result =
				s2sdk::HookEvent(plg::string(hook.name), hook.callback, s2sdk::HookMode::Post);
			if (result != s2sdk::EventHookError::Okay) {
				LogWarning(std::string("Could not hook game event '") + hook.name + "'");
			} else {
				hook.hooked = true;
				any = true;
			}
		} catch (...) {
			// The binding may have registered the hook before throwing: detach it best-effort.
			if (__s2sdk_UnhookEvent != nullptr) {
				try {
					s2sdk::UnhookEvent(plg::string(hook.name), hook.callback, s2sdk::HookMode::Post);
				} catch (...) {
				}
			}
		}
	}
	g_eventsHooked = any;
}

void UnhookEvents() {
	if (!g_eventsHooked) {
		for (EventHook& hook : kEventHooks) {
			hook.hooked = false;
		}
		return;
	}
	for (EventHook& hook : kEventHooks) {
		if (!hook.hooked) {
			continue;
		}
		hook.hooked = false;
		if (__s2sdk_UnhookEvent == nullptr) {
			continue;
		}
		try {
			s2sdk::UnhookEvent(plg::string(hook.name), hook.callback, s2sdk::HookMode::Post);
		} catch (...) {
		}
	}
	g_eventsHooked = false;
}

void OnMapStartListener() {
	// A callback of s2sdk: nothing may unwind out of here. Only state is touched before the gate
	// opens; afterwards the map name, game directory and cvars are safe engine queries.
	try {
		g_mapGate.OnMapStart();
		g_mapInitialized = false;
		HandleMapReady();
	} catch (...) {
	}
}

void OnMapEndListener() {
	// PITFALLS B2: only state changes here - no client, entity or schema call. StopRecording uses
	// the cached game time, player count and server name.
	try {
		g_mapGate.OnMapEnd();
		g_mapInitialized = false;
		StopRecording();
		g_currentMap.clear();
		g_warmupPeriodOffset = -1;
		DisableIdleChecks();
		g_idleAccumulator = 0.0;
		g_cleanupAccumulator = 0.0;
		g_retentionAccumulator = 0.0;
	} catch (...) {
	}
}

void UnregisterMapListenersBestEffort() {
	if (__s2sdk_OnMapStart_Unregister != nullptr) {
		try {
			s2sdk::OnMapStart_Unregister(&OnMapStartListener);
		} catch (...) {
		}
	}
	if (__s2sdk_OnMapEnd_Unregister != nullptr) {
		try {
			s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
		} catch (...) {
		}
	}
}

void RegisterMapListeners() {
	if (g_mapGate.ListenersRegistered()) {
		return;
	}
	// The paired Register+Unregister set is checked before anything is registered: a listener that
	// cannot be unregistered would point into this library after unload (PITFALLS A7).
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr
		|| __s2sdk_OnMapStart_Unregister == nullptr || __s2sdk_OnMapEnd_Unregister == nullptr) {
		// Without the map-end listener nothing could take readiness away again, so the gate stays
		// closed for good (PITFALLS A1: log and stay inactive, never return Failed).
		LogError("The s2sdk map listener pair is unavailable - the plugin stays inactive.");
		return;
	}
	try {
		s2sdk::OnMapStart_Register(&OnMapStartListener);
		s2sdk::OnMapEnd_Register(&OnMapEndListener);
	} catch (const std::exception& exception) {
		UnregisterMapListenersBestEffort();
		LogError(std::string("registering the map listeners failed: ") + exception.what());
		return;
	} catch (...) {
		UnregisterMapListenersBestEffort();
		LogError("registering the map listeners failed");
		return;
	}
	g_mapGate.SetListenersRegistered(true);
}

void UnregisterMapListeners() {
	if (!g_mapGate.ListenersRegistered()) {
		return;
	}
	g_mapGate.SetListenersRegistered(false);
	if (__s2sdk_OnMapStart_Unregister != nullptr) {
		try {
			s2sdk::OnMapStart_Unregister(&OnMapStartListener);
		} catch (...) {
		}
	}
	if (__s2sdk_OnMapEnd_Unregister != nullptr) {
		try {
			s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
		} catch (...) {
		}
	}
}

// --- configuration -----------------------------------------------------------------------------

void StoreConfigMtime() {
	std::error_code error;
	const auto stamp = std::filesystem::last_write_time(g_configPath, error);
	if (!error) {
		g_configMtime = stamp;
		g_haveConfigMtime = true;
	}
}

// Returns false when the file could not be read or parsed. On the first load the defaults are used
// then; on a hot reload the *previous* configuration is kept, because a save that is still in
// progress (or a typo) must not switch recording/Discord/S3 off in the middle of a demo (the C#
// IOptionsMonitor keeps serving the last valid values the same way).
bool LoadConfig(bool initial) {
	bool ok = false;
	const std::string text = ReadFile(g_configPath, &ok);
	if (!ok) {
		LogError("cannot read '" + g_configPath + "'" + (initial ? "; the built-in defaults are used" : "; the previous configuration stays active"));
		if (initial) {
			g_config = k4::DefaultConfig();
			k4::NormalizeConfig(g_config);
		}
		return false;
	}
	k4::Config parsed;
	std::string warning;
	if (!k4::ParseConfig(text, parsed, warning)) {
		LogError(warning + (initial ? "; the built-in defaults are used" : "; the previous configuration stays active"));
		if (initial) {
			g_config = k4::DefaultConfig();
			k4::NormalizeConfig(g_config);
		}
		return false;
	}
	if (!warning.empty()) {
		LogWarning(warning);
	}
	g_config = parsed;
	k4::NormalizeConfig(g_config);
	return true;
}

void ResolveDemoDirectory() {
	if (!g_mapGate.Ready()) {
		return;
	}
	// <game dir>/<General.DemoDirectory>, the original's Path.Combine(Core.CSGODirectory, ...).
	const std::string gameDirectory = GameDirectory();
	std::string demoDirectory = g_config.general.demoDirectory;
	// Like Path.Combine: an absolute DemoDirectory wins over the game directory.
	if (!gameDirectory.empty() && (demoDirectory.empty() || demoDirectory.front() != '/')) {
		demoDirectory = JoinPath(gameDirectory, demoDirectory);
	}
	g_demoDir = demoDirectory;
	if (!g_demoDir.empty()) {
		std::error_code error;
		std::filesystem::create_directories(g_demoDir, error);
	}
}

void HandleMapReady() {
	if (g_mapInitialized || !g_mapGate.Ready()) {
		return;
	}
	g_mapInitialized = true;
	g_currentMap = CurrentMapName();
	g_roundNumber = 1;
	g_lastPlayerCount = 0;
	g_warmupPeriodOffset = -1;
	RefreshServerName();
	ResolveDemoDirectory();
	if (!g_demoDir.empty()) {
		LogInfo("Demo directory: " + g_demoDir);
	}
	if (g_config.general.deleteEveryDemoFromServerAfterServerStart || g_pendingStartupCleanup) {
		g_pendingStartupCleanup = false;
		PostStartupCleanup();
	}
}

void ApplyConfiguration(bool firstLoad) {
	ResolveDemoDirectory();

	k4::HttpTransport transport = k4::HttpTransport::Auto;
	if (g_config.http.transport == "steam") {
		transport = k4::HttpTransport::Steam;
	} else if (g_config.http.transport == "curl") {
		transport = k4::HttpTransport::Curl;
	}
	g_http.Configure(transport, g_config.http.caFile);

	g_discord.Configure(g_config.discord, &g_http, &g_worker);
	if (firstLoad || !g_s3.Pending()) {
		g_s3.Configure(g_config.s3, &g_http, &g_worker);
		g_s3ConfigPending = false;
	} else {
		// Re-applied from OnPluginUpdate as soon as the pending upload/retention pass ends, so an
		// operator fixing credentials while an upload is stuck is not silently ignored.
		g_s3ConfigPending = true;
		LogInfo("S3 configuration changed; it applies as soon as the current upload/retention pass finishes.");
	}
}

void CheckConfigReload() {
	static std::chrono::steady_clock::time_point lastCheck {};
	const auto now = std::chrono::steady_clock::now();
	if (now - lastCheck < std::chrono::seconds(1)) {
		return;
	}
	lastCheck = now;
	std::error_code error;
	const auto stamp = std::filesystem::last_write_time(g_configPath, error);
	if (error) {
		return;
	}
	if (g_haveConfigMtime && stamp == g_configMtime) {
		return;
	}
	g_configMtime = stamp;
	g_haveConfigMtime = true;
	if (g_configPath.empty()) {
		return;
	}
	LogInfo("config.jsonc changed; reloading");
	if (!LoadConfig(false)) {
		return; // keep the previous config (a partially written file or a typo)
	}
	LoadPayloadTemplate();
	ApplyConfiguration(false);
	ApplyReloadableSettings();
}

void LoadPayloadTemplate() {
	bool ok = false;
	const std::string path = JoinPath(ConfigDir(), "payload.json");
	g_payloadTemplate = ReadFile(path, &ok);
	if (!ok) {
		g_payloadTemplate.clear();
		if (!g_payloadMissingLogged) {
			g_payloadMissingLogged = true;
			LogWarning("Discord payload template not found: " + path);
		}
	}
}

void StartTimers() {
	// No s2sdk timer exists in this plugin (PITFALLS C9): the two long-lived intervals are update
	// accumulators. A fresh config starts them from zero, like the recreated pisex timers did.
	g_cleanupAccumulator = 0.0;
	g_retentionAccumulator = 0.0;
}

void StopTimers() {
	DisableIdleChecks();
	g_cleanupAccumulator = 0.0;
	g_retentionAccumulator = 0.0;
}

// The README promises that config values are re-read when the file changes: options that only act
// through a registration or a long-lived timer need this after a hot reload (the callbacks already
// re-check their enable flags, but a toggle must also create or drop the corresponding work).
void ApplyReloadableSettings() {
	ApplyDemoCommandSetting();
	// Reset the long-lived accumulators: this applies an enable/disable toggle and an interval
	// change, and a dropped option must not leave work ticking for ever.
	g_cleanupAccumulator = 0.0;
	g_retentionAccumulator = 0.0;
	StartTimers();
	// Latching happens only when the pass is actually posted, so enabling the option on a live
	// config runs it at the next reload instead of waiting for a process restart.
	if (g_config.general.deleteEveryDemoFromServerAfterServerStart) {
		if (g_mapGate.Ready()) {
			PostStartupCleanup();
		} else {
			g_pendingStartupCleanup = true;
		}
	}
	if (g_recording.active) {
		if (g_config.autoRecord.stopOnIdle) {
			StartIdleTimer();
		} else {
			DisableIdleChecks();
		}
	}
}

// --- reset / plugin ----------------------------------------------------------------------------

void ShutdownServices() {
	// Every step is independent: a failure in one service must not keep the file worker or the
	// plgdb thread alive after the library is unloaded (PITFALLS A7 - no callbacks into freed code).
	try {
		g_worker.Stop();
	} catch (...) {
		LogError("stopping the file worker failed");
	}
	try {
		g_http.Shutdown();
	} catch (...) {
		LogError("stopping the HTTP service failed");
	}
	try {
		g_s3.Shutdown();
	} catch (...) {
		LogError("stopping the S3 client failed");
	}
	try {
		g_discord.Shutdown();
	} catch (...) {
		LogError("stopping the Discord service failed");
	}
	try {
		g_database.Shutdown();
	} catch (...) {
		LogError("stopping the database service failed");
	}
}

void ResetAllState() {
	g_config = k4::DefaultConfig();
	g_configPath.clear();
	g_configMtime = {};
	g_haveConfigMtime = false;
	g_demoDir.clear();
	g_currentMap.clear();
	g_roundNumber = 1;
	g_lastPlayerCount = 0;
	g_lastGameTime = 0.0;
	g_serverName = "Unknown Server";
	g_tvDisabledWarned = false;
	g_warmupPeriodOffset = -1;
	ResetRecordingState();
	g_requesters.clear();
	DisableIdleChecks();
	g_cleanupAccumulator = 0.0;
	g_retentionAccumulator = 0.0;
	g_pendingIdleStop = false;
	g_pendingRoundStartRecording = false;
	g_pendingDemos.clear();
	g_translations.clear();
	g_payloadTemplate.clear();
	g_payloadMissingLogged = false;
	g_eventsHooked = false;
	for (EventHook& hook : kEventHooks) {
		hook.hooked = false;
	}
	g_initialRecordingChecked = false;
	g_s3ConfigPending = false;
	g_startupCleanupDone = false;
	g_pendingStartupCleanup = false;
	g_demoCommandRegistered = false;
	g_demoCommandOwned = false;
	g_commandBindingsWarned = false;
	g_unloading = false;
	g_mapInitialized = false;
	g_mapGate.Reset();
}

class K4GotvPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			ResetAllState();
			g_configPath = JoinPath(ConfigDir(), "config.jsonc");
			LoadConfig(true);
			LoadTranslations();
			LoadPayloadTemplate();
			g_worker.Start();
			ApplyConfiguration(true);
			StoreConfigMtime();

			bool fallback = false;
			const plgdb::Config database = k4::LoadDatabaseConfig(&fallback);
			if (fallback) {
				LogInfo("No 'k4_gotv' section in databases.cfg; using the local SQLite fallback.");
			}
			g_database.Configure(database, fallback);

			RegisterMapListeners();
			HookEvents();
			// DemoRequest.Enabled forces AutoRecord on (the original config getter); when it is
			// off, the command does not exist, exactly like the original.
			ApplyDemoCommandSetting();
			StartTimers();

			LogInfo("loaded; AutoRecord " + std::string(g_config.autoRecord.enabled ? "on" : "off")
					+ ", crop rounds " + (g_config.autoRecord.cropRounds ? "on" : "off") + ", S3 "
					+ (g_s3.Configured() ? "on" : "off") + ", Discord " + (g_discord.Enabled() ? "on" : "off")
					+ ", HTTP transport " + g_config.http.transport + ", database "
					+ (g_database.Enabled() ? "on" : "off"));
		} catch (const std::exception& exception) {
			LogError(std::string("OnPluginStart: ") + exception.what());
		} catch (...) {
			LogError("OnPluginStart failed");
		}
		return {}; // never Failed: one Failed stops every Plugify plugin on the server
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override {
		try {
			if (g_unloading) {
				return {};
			}
			const double dtSeconds = std::max(0.0, static_cast<double>(dt.count()) / 1000.0);

			g_worker.Poll();
			g_http.Pump();
			g_s3.Pump();
			g_discord.Pump();
			g_database.Poll();
			CheckConfigReload();
			if (g_s3ConfigPending && !g_s3.Pending()) {
				g_s3ConfigPending = false;
				g_s3.Configure(g_config.s3, &g_http, &g_worker);
				LogInfo("Deferred S3 configuration applied.");
			}

			// B1/B2: while the gate is closed only the map-name engine query is made (the mid-map
			// latch needs it); no client, entity or schema call is reachable from here.
			if (!g_mapGate.Ready()) {
				const std::string map = CurrentMapName();
				if (g_mapGate.OnPluginUpdate(map)) {
					HandleMapReady();
				}
			}
			if (!g_mapGate.Ready()) {
				return {};
			}
			// Cache the engine time while the gate is open; StopRecording at OnMapEnd uses this
			// value instead of touching gpGlobals after the map is gone (B2).
			RefreshGameTime();
			if (g_currentMap.empty()) {
				g_currentMap = CurrentMapName();
			}

			// The original starts recording on a hot reload when players are already online
			// (Plugin.cs Load(hotReload)); this also covers a plugin loaded into a running map.
			if (!g_initialRecordingChecked) {
				g_initialRecordingChecked = true;
				if (g_config.autoRecord.enabled && !g_recording.active && RealPlayerCount() > 0) {
					StartRecording("autodemo");
				}
			}

			UpdateIdleCheck(dtSeconds);
			UpdateAutoCleanup(dtSeconds);
			UpdateRetention(dtSeconds);
			ProcessPendingDemos();
			ProcessDeferredActions();

			// Keep the cached player count fresh while a demo is recording: the map-end stop
			// must not query clients (PITFALLS B2), and its embed/database row uses that cache.
			static std::chrono::steady_clock::time_point lastPlayerTally {};
			const auto now = std::chrono::steady_clock::now();
			if (g_recording.active && now - lastPlayerTally >= std::chrono::seconds(1)) {
				lastPlayerTally = now;
				RealPlayerCount();
			}
		} catch (const std::exception& exception) {
			LogError(std::string("OnPluginUpdate: ") + exception.what());
		} catch (...) {
			LogError("OnPluginUpdate failed");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			g_unloading = true;
			UnregisterMapListeners();
			UnhookEvents();
			UnregisterDemoCommand();
			StopRecording(/*scheduleProcessing=*/false);
			StopTimers();
			g_pendingDemos.clear();
			g_pendingIdleStop = false;
			g_pendingRoundStartRecording = false;
			g_pendingStartupCleanup = false;
			// Order matters: the worker is drained first (its completions are dropped, no further
			// HTTP request can be queued), then the HTTP service joins its curl thread, then the
			// services drop their state and plgdb joins its own worker. Each step is guarded, so a
			// single failure cannot leave a background thread behind on unload.
			ShutdownServices();
			g_mapGate.Close();
			g_mapGate.SetListenersRegistered(false);
			g_currentMap.clear();
			g_demoDir.clear();
			g_requesters.clear();
			g_translations.clear();
			g_payloadTemplate.clear();
			g_startupCleanupDone = false;
		} catch (const std::exception& exception) {
			LogError(std::string("OnPluginEnd: ") + exception.what());
		} catch (...) {
			LogError("OnPluginEnd failed");
		}
		return {};
	}
};

K4GotvPlugin g_k4GotvPlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_k4GotvPlugin)
