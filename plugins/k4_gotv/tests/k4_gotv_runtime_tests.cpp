// k4_gotv — fake-runtime test of the *real* src/plugin.cpp.
//
// src/plugin.cpp is compiled into this executable together with the generated s2sdk binding
// objects. Their __s2sdk_* globals are plain variables that the plugify module normally fills; here
// the test installs recording fakes, drives the plugin through its own exported entry points
// (Plugify_PluginStart / _Update / _End) and fires the callbacks the plugin registered.
//
// What it proves:
//   * B1/B2: OnPluginStart makes no client/entity/schema call; the gate stays shut before a map,
//     opens on OnMapStart, closes on OnMapEnd and the updates the host keeps pumping through a
//     changelevel never touch a client; the mid-map latch opens on the first update only with a
//     real s2sdk::GetCurrentMap() value and all four map listeners registered;
//   * A7: OnPluginEnd unregisters the map listeners, every event hook and the `demo` command with
//     the same callbacks/mode, joins the file worker and shuts plgdb down (no crash on reload);
//   * C9: recording start/stop and the idle/cleanup/retention work run from OnPluginUpdate; the
//     plugin registers no s2sdk timer at all;
//   * C11/C13: the `demo` command answers chat/client-console callers and ignores the server
//     console (caller -1); every Pre/Post event hook returns Continue; player event fields are read
//     with GetEventPlayerSlot and never with GetEventInt;
//   * C2/C1: the controller pointer/handle are validated, 0/-1 handles are refused and a negative
//     high-serial handle is accepted;
//   * databases.cfg is resolved in the standard `<configs>/k4_gotv/databases.cfg` location and a
//     SQLite `sqlite_path` relative to `<data>/k4_gotv` is created (no fallback log).
//
// Nothing here talks to a game server: every binding is a counting fake in this file.

#include <plg/plugin.hpp>
#include <plg/source_location.hpp>
#include <plg/string.hpp>

#include <s2sdk/s2sdk.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <unistd.h>

extern "C" plg::PluginResult Plugify_PluginStart();
extern "C" plg::PluginResult Plugify_PluginUpdate(std::chrono::milliseconds dt);
extern "C" plg::PluginResult Plugify_PluginEnd();

namespace {

int g_checks = 0;
int g_failures = 0;

void CheckImpl(bool condition, std::string_view what, int line) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL %s:%d: %.*s\n", __FILE__, line, static_cast<int>(what.size()), what.data());
	}
}

template <typename T>
void CheckEqImpl(const T& actual, const T& expected, std::string_view what, int line) {
	++g_checks;
	if (!(actual == expected)) {
		++g_failures;
		std::printf("FAIL %s:%d: %.*s\n", __FILE__, line, static_cast<int>(what.size()), what.data());
	}
}

#define CHECK(condition) CheckImpl((condition), #condition, __LINE__)
#define CHECK_EQ(actual, expected) CheckEqImpl((actual), (expected), #actual " == " #expected, __LINE__)

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

plg::string ToPlg(const std::string& text) {
	return plg::string(text.data(), text.size());
}

// --- what the fakes answer ---------------------------------------------------------------------

struct FakeClient {
	bool connected = false;
	bool inGame = false;
	bool sourceTv = false;
	bool fake = false;
	bool controller = true;
	std::string name = "Player";
	uint64_t steam = 76561198000000000ull;
	std::string language = "english";
	int32_t handle = 100;
};

struct FakeState {
	std::array<FakeClient, 64> clients{};
	std::string currentMap = "<empty>";
	std::string gameDirectory;
	float gameTime = 100.0f;
	int32_t totalRounds = 0;
	std::string serverName = "Test Server";
	int eventSlot = -1;
	bool throwOnClient = false;
	bool throwOnServerCommand = false;
};

FakeState g_fake;

std::vector<int> g_clientCalls;
std::vector<int> g_handleCalls;
std::vector<std::string> g_eventSlotKeys;
std::vector<std::string> g_eventIntKeys;
std::vector<std::string> g_serverCommands;
std::vector<std::pair<int, std::string>> g_printed;
std::vector<std::string> g_errors;
std::vector<std::string> g_warnings;
std::vector<std::string> g_infos;

struct HookRecord {
	std::string name;
	s2sdk::EventCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
};

std::vector<HookRecord> g_events;       // active hooks
std::vector<HookRecord> g_hookCalls;    // every HookEvent call
std::vector<HookRecord> g_unhookCalls;  // every UnhookEvent call

struct CommandRecord {
	std::string name;
	s2sdk::ConVarFlag flags = static_cast<s2sdk::ConVarFlag>(0);
	s2sdk::ConCommandCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
};
std::vector<CommandRecord> g_commands;       // active commands/listeners
std::vector<CommandRecord> g_commandAdds;    // every AddConsoleCommand call
std::vector<std::string> g_commandRemoves;   // RemoveCommand calls
std::vector<std::string> g_listenerRemoves;  // RemoveCommandListener calls
bool g_commandOwned = true;
int g_timerCalls = 0;

s2sdk::OnMapStartCallback g_onMapStart = nullptr;
s2sdk::OnMapEndCallback g_onMapEnd = nullptr;
bool g_mapStartRegistered = false;
bool g_mapEndRegistered = false;
bool g_mapStartUnregistered = false;
bool g_mapEndUnregistered = false;

std::string g_configsDir;
std::string g_dataDir;

void ResetCalls() {
	g_clientCalls.clear();
	g_handleCalls.clear();
	g_eventSlotKeys.clear();
	g_eventIntKeys.clear();
	g_serverCommands.clear();
	g_printed.clear();
	g_errors.clear();
	g_warnings.clear();
	g_infos.clear();
	g_timerCalls = 0;
}

bool Contains(const std::vector<std::string>& values, std::string_view wanted) {
	return std::find(values.begin(), values.end(), wanted) != values.end();
}

// --- plg host fakes ----------------------------------------------------------------------------

plg::string FakeGetConfigsDir() {
	return ToPlg(g_configsDir);
}

plg::string FakeGetDataDir() {
	return ToPlg(g_dataDir);
}

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location&) {
	const std::string text(message);
	if (severity == plg::Severity::Error || severity == plg::Severity::Fatal) {
		g_errors.push_back(text);
	} else if (severity == plg::Severity::Warning) {
		g_warnings.push_back(text);
	} else if (severity == plg::Severity::Info) {
		g_infos.push_back(text);
	}
}

// --- s2sdk client fakes ------------------------------------------------------------------------

bool FakeIsClientConnected(int32_t slot) {
	g_clientCalls.push_back(slot);
	if (g_fake.throwOnClient) {
		throw std::runtime_error("fake client failure");
	}
	return slot >= 0 && slot < 64 && g_fake.clients[static_cast<std::size_t>(slot)].connected;
}

bool FakeIsClientInGame(int32_t slot) {
	if (g_fake.throwOnClient) {
		throw std::runtime_error("fake client failure");
	}
	return slot >= 0 && slot < 64 && g_fake.clients[static_cast<std::size_t>(slot)].inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	if (g_fake.throwOnClient) {
		throw std::runtime_error("fake client failure");
	}
	return slot >= 0 && slot < 64 && g_fake.clients[static_cast<std::size_t>(slot)].sourceTv;
}

bool FakeIsFakeClient(int32_t slot) {
	if (g_fake.throwOnClient) {
		throw std::runtime_error("fake client failure");
	}
	return slot >= 0 && slot < 64 && g_fake.clients[static_cast<std::size_t>(slot)].fake;
}

int32_t FakeGetMaxClients() {
	return 64;
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	g_handleCalls.push_back(slot);
	if (slot < 0 || slot >= 64 || !g_fake.clients[static_cast<std::size_t>(slot)].controller) {
		return nullptr;
	}
	static std::array<char, 64> tokens{};
	return &tokens[static_cast<std::size_t>(slot)];
}

int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	if (slot < 0 || slot >= 64) {
		return -1;
	}
	return g_fake.clients[static_cast<std::size_t>(slot)].handle;
}

bool FakeIsValidEntPointer(void* pointer) {
	return pointer != nullptr;
}

bool FakeIsValidEntHandle(int32_t handle) {
	return handle != 0 && handle != -1;
}

plg::string FakeGetClientName(int32_t slot) {
	if (slot < 0 || slot >= 64) {
		return ToPlg("<unknown>");
	}
	return ToPlg(g_fake.clients[static_cast<std::size_t>(slot)].name);
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	if (slot < 0 || slot >= 64) {
		return 0;
	}
	return g_fake.clients[static_cast<std::size_t>(slot)].steam;
}

plg::string FakeGetClientLanguage(int32_t slot) {
	if (slot < 0 || slot >= 64) {
		return {};
	}
	return ToPlg(g_fake.clients[static_cast<std::size_t>(slot)].language);
}

float FakeGetGameTime() {
	return g_fake.gameTime;
}

plg::string FakeGetCurrentMap() {
	return ToPlg(g_fake.currentMap);
}

plg::string FakeGetGameDirectory() {
	return ToPlg(g_fake.gameDirectory);
}

uint64_t FakeFindConVar(const plg::string& name) {
	return ToStd(name).empty() ? 0 : 1;
}

bool FakeGetConVarBool(uint64_t handle) {
	return handle != 0;
}

plg::string FakeGetConVarString(uint64_t handle) {
	return handle != 0 ? ToPlg(g_fake.serverName) : plg::string();
}

void* FakeGetGameRules() {
	static int rules = 0;
	return &rules;
}

int32_t FakeGetGameTotalRoundsPlayed() {
	return g_fake.totalRounds;
}

int32_t FakeGetSchemaOffset(const plg::string&, const plg::string&) {
	return -1;
}

int32_t FakeGetEventPlayerSlot(void*, const plg::string& key) {
	g_eventSlotKeys.push_back(ToStd(key));
	return g_fake.eventSlot;
}

int32_t FakeGetEventInt(void*, const plg::string& key) {
	g_eventIntKeys.push_back(ToStd(key));
	return -1;
}

void FakePrintToChat(int32_t slot, const plg::string& message) {
	g_printed.emplace_back(slot, ToStd(message));
}

void FakeServerCommand(const plg::string& command) {
	if (g_fake.throwOnServerCommand) {
		throw std::runtime_error("fake server command failure");
	}
	g_serverCommands.push_back(ToStd(command));
}

bool FakeAddConsoleCommand(const plg::string& name, const plg::string&, s2sdk::ConVarFlag flags,
						   s2sdk::ConCommandCallback callback, s2sdk::HookMode mode) {
	CommandRecord record;
	record.name = ToStd(name);
	record.flags = flags;
	record.callback = callback;
	record.mode = mode;
	g_commands.push_back(record);
	g_commandAdds.push_back(record);
	return g_commandOwned;
}

bool FakeRemoveCommand(const plg::string& name, s2sdk::ConCommandCallback callback) {
	g_commandRemoves.push_back(ToStd(name));
	g_commands.erase(std::remove_if(g_commands.begin(), g_commands.end(),
									[&](const CommandRecord& record) {
										return record.name == ToStd(name) && record.callback == callback;
									}),
					 g_commands.end());
	return true;
}

bool FakeRemoveCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback, s2sdk::HookMode) {
	g_listenerRemoves.push_back(ToStd(name));
	g_commands.erase(std::remove_if(g_commands.begin(), g_commands.end(),
									[&](const CommandRecord& record) {
										return record.name == ToStd(name) && record.callback == callback;
									}),
					 g_commands.end());
	return true;
}

s2sdk::TimerId FakeCreateTimer(double, s2sdk::TimerCallback, s2sdk::TimerFlag, const plg::vector<plg::any>&) {
	++g_timerCalls;
	return static_cast<s2sdk::TimerId>(-1);
}

void FakeKillsTimer(s2sdk::TimerId) {
	++g_timerCalls;
}

void FakeRescheduleTimer(s2sdk::TimerId, double) {
	++g_timerCalls;
}

s2sdk::EventHookError FakeHookEvent(const plg::string& name, s2sdk::EventCallback callback, s2sdk::HookMode mode) {
	HookRecord record{ToStd(name), callback, mode};
	g_events.push_back(record);
	g_hookCalls.push_back(record);
	return s2sdk::EventHookError::Okay;
}

s2sdk::EventHookError FakeUnhookEvent(const plg::string& name, s2sdk::EventCallback callback, s2sdk::HookMode mode) {
	HookRecord record{ToStd(name), callback, mode};
	g_unhookCalls.push_back(record);
	g_events.erase(std::remove_if(g_events.begin(), g_events.end(),
								  [&](const HookRecord& active) {
									  return active.name == record.name && active.callback == callback
											 && active.mode == mode;
								  }),
				   g_events.end());
	return s2sdk::EventHookError::Okay;
}

void FakeOnMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_onMapStart = callback;
	g_mapStartRegistered = true;
}

void FakeOnMapStartUnregister(s2sdk::OnMapStartCallback) {
	g_mapStartRegistered = false;
	g_mapStartUnregistered = true;
	g_onMapStart = nullptr;
}

void FakeOnMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_onMapEnd = callback;
	g_mapEndRegistered = true;
}

void FakeOnMapEndUnregister(s2sdk::OnMapEndCallback) {
	g_mapEndRegistered = false;
	g_mapEndUnregistered = true;
	g_onMapEnd = nullptr;
}

// --- binding installation ----------------------------------------------------------------------

void InstallBindings() {
	plg::GetConfigsDir = &FakeGetConfigsDir;
	plg::GetDataDir = &FakeGetDataDir;
	plg::Log = &FakeLog;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_GetMaxClients = &FakeGetMaxClients;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_GetClientName = &FakeGetClientName;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_GetClientLanguage = &FakeGetClientLanguage;
	__s2sdk_GetGameTime = &FakeGetGameTime;
	__s2sdk_GetCurrentMap = &FakeGetCurrentMap;
	__s2sdk_GetGameDirectory = &FakeGetGameDirectory;
	__s2sdk_FindConVar = &FakeFindConVar;
	__s2sdk_GetConVarBool = &FakeGetConVarBool;
	__s2sdk_GetConVarString = &FakeGetConVarString;
	__s2sdk_GetGameRules = &FakeGetGameRules;
	__s2sdk_GetGameTotalRoundsPlayed = &FakeGetGameTotalRoundsPlayed;
	__s2sdk_GetSchemaOffset = &FakeGetSchemaOffset;
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;
	__s2sdk_GetEventInt = &FakeGetEventInt;
	__s2sdk_PrintToChat = &FakePrintToChat;
	__s2sdk_ServerCommand = &FakeServerCommand;
	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
	__s2sdk_RemoveCommand = &FakeRemoveCommand;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;
	__s2sdk_HookEvent = &FakeHookEvent;
	__s2sdk_UnhookEvent = &FakeUnhookEvent;
	__s2sdk_OnMapStart_Register = &FakeOnMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeOnMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeOnMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;
	__s2sdk_CreateTimer = &FakeCreateTimer;
	__s2sdk_KillsTimer = &FakeKillsTimer;
	__s2sdk_RescheduleTimer = &FakeRescheduleTimer;
}

void ClearBindings() {
	__s2sdk_IsClientConnected = nullptr;
	__s2sdk_IsClientInGame = nullptr;
	__s2sdk_IsClientSourceTV = nullptr;
	__s2sdk_IsFakeClient = nullptr;
	__s2sdk_GetMaxClients = nullptr;
	__s2sdk_PlayerSlotToEntPointer = nullptr;
	__s2sdk_PlayerSlotToEntHandle = nullptr;
	__s2sdk_IsValidEntPointer = nullptr;
	__s2sdk_IsValidEntHandle = nullptr;
	__s2sdk_GetClientName = nullptr;
	__s2sdk_GetClientSteamID64 = nullptr;
	__s2sdk_GetClientLanguage = nullptr;
	__s2sdk_GetGameTime = nullptr;
	__s2sdk_GetCurrentMap = nullptr;
	__s2sdk_GetGameDirectory = nullptr;
	__s2sdk_FindConVar = nullptr;
	__s2sdk_GetConVarBool = nullptr;
	__s2sdk_GetConVarString = nullptr;
	__s2sdk_GetGameRules = nullptr;
	__s2sdk_GetGameTotalRoundsPlayed = nullptr;
	__s2sdk_GetSchemaOffset = nullptr;
	__s2sdk_GetEventPlayerSlot = nullptr;
	__s2sdk_GetEventInt = nullptr;
	__s2sdk_PrintToChat = nullptr;
	__s2sdk_ServerCommand = nullptr;
	__s2sdk_AddConsoleCommand = nullptr;
	__s2sdk_RemoveCommand = nullptr;
	__s2sdk_RemoveCommandListener = nullptr;
	__s2sdk_HookEvent = nullptr;
	__s2sdk_UnhookEvent = nullptr;
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapStart_Unregister = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	__s2sdk_OnMapEnd_Unregister = nullptr;
	__s2sdk_CreateTimer = nullptr;
	__s2sdk_KillsTimer = nullptr;
	__s2sdk_RescheduleTimer = nullptr;
}

void ResetRuntime() {
	ResetCalls();
	g_events.clear();
	g_hookCalls.clear();
	g_unhookCalls.clear();
	g_commands.clear();
	g_commandAdds.clear();
	g_commandRemoves.clear();
	g_listenerRemoves.clear();
	g_commandOwned = true;
	g_onMapStart = nullptr;
	g_onMapEnd = nullptr;
	g_mapStartRegistered = false;
	g_mapEndRegistered = false;
	g_mapStartUnregistered = false;
	g_mapEndUnregistered = false;
	g_fake = FakeState {};
	g_fake.gameDirectory = g_configsDir + "/game";
}

// --- filesystem fixture ------------------------------------------------------------------------

void WriteFile(const std::string& path, std::string_view text) {
	std::filesystem::create_directories(std::filesystem::path(path).parent_path());
	std::ofstream out(path, std::ios::binary);
	out << text;
}

void CreateFixture() {
	const std::string pluginConfigs = g_configsDir + "/k4_gotv";
	WriteFile(pluginConfigs + "/config.jsonc", R"jsonc(
{
	"General": {
		"MinimumDemoDuration": 0.0,
		"DeleteDemoAfterUpload": true,
		"DeleteZippedDemoAfterUpload": true,
		"DemoDirectory": "demos",
		"AutoCleanupEnabled": false
	},
	"AutoRecord": {
		"Enabled": true,
		"CropRounds": true,
		"StopOnIdle": false,
		"RecordWarmup": true
	},
	"DemoRequest": {
		"Enabled": true,
		"PrintAll": true,
		"DeleteUnused": true
	},
	"Discord": { "WebhookURL": "" },
	"S3": { "access_key": "", "secret_key": "" }
})jsonc");
	WriteFile(pluginConfigs + "/payload.json", "{}");
	WriteFile(pluginConfigs + "/translations/en.jsonc", R"jsonc(
{
	"k4.general.prefix": "EN",
	"k4.chat.demo.request.all": "all {0}",
	"k4.chat.demo.request.self": "self"
})jsonc");
	WriteFile(pluginConfigs + "/translations/pt-BR.jsonc", R"jsonc(
{
	"k4.general.prefix": "PT",
	"k4.chat.demo.request.all": "todos {0}",
	"k4.chat.demo.request.self": "proprio"
})jsonc");
	WriteFile(pluginConfigs + "/databases.cfg", R"kv(
"Databases"
{
	"k4_gotv"
	{
		"driver"      "sqlite"
		"sqlite_path" "db/custom.sqlite3"
	}
}
)kv");
}

void RemoveFixture() {
	std::error_code error;
	std::filesystem::remove_all(g_configsDir, error);
	std::filesystem::remove_all(g_dataDir, error);
}

// --- helpers to drive the plugin ---------------------------------------------------------------

void Update(std::chrono::milliseconds dt = std::chrono::milliseconds(16)) {
	(void)Plugify_PluginUpdate(dt);
}

void FireMapStart() {
	if (g_onMapStart != nullptr) {
		g_onMapStart();
	}
}

void FireMapEnd() {
	if (g_onMapEnd != nullptr) {
		g_onMapEnd();
	}
}

s2sdk::EventCallback FindHook(const std::string& name) {
	for (const HookRecord& hook : g_events) {
		if (hook.name == name) {
			return hook.callback;
		}
	}
	return nullptr;
}

bool FireEvent(const std::string& name) {
	const s2sdk::EventCallback callback = FindHook(name);
	if (callback == nullptr) {
		return false;
	}
	static int event = 0;
	plg::string eventName = ToPlg(name);
	return callback(eventName, &event, false) == s2sdk::ResultType::Continue;
}

const CommandRecord* FindCommand(const std::string& name) {
	for (const CommandRecord& command : g_commands) {
		if (command.name == name) {
			return &command;
		}
	}
	return nullptr;
}

int CountServerCommands(std::string_view prefix) {
	int count = 0;
	for (const std::string& command : g_serverCommands) {
		if (command.rfind(prefix, 0) == 0) {
			++count;
		}
	}
	return count;
}

// --- scenarios ---------------------------------------------------------------------------------

void TestStartBeforeMapGate() {
	ResetRuntime();
	InstallBindings();
	g_fake.currentMap = "<empty>";
	g_fake.clients[0].connected = true;
	g_fake.clients[0].inGame = true;
	g_fake.clients[0].controller = true;

	CHECK(Plugify_PluginStart().GetCode() == plg::PluginCode::Ok); // the result is always success
	CHECK(g_errors.empty());
	// No map listener missing, no fallback: the standard databases.cfg section was used.
	CHECK(g_mapStartRegistered && g_mapEndRegistered);
	CHECK(g_commands.size() == 1);
	CHECK(g_commandAdds.front().name == "demo");
	CHECK(g_commandAdds.front().mode == s2sdk::HookMode::Pre);
	const int64_t flagBits = static_cast<int64_t>(g_commandAdds.front().flags);
	CHECK((flagBits & static_cast<int64_t>(s2sdk::ConVarFlag::Release)) != 0);
	CHECK((flagBits & static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute)) != 0);
	// C9: the plugin must not register an s2sdk timer at all.
	CHECK(g_timerCalls == 0);
	CHECK(g_events.size() == 3);
	CHECK(g_events[0].mode == s2sdk::HookMode::Post);

	// OnPluginStart must not touch a client/entity/schema binding.
	CHECK(g_clientCalls.empty());
	CHECK(g_handleCalls.empty());

	// First update with no map: the one-shot latch refuses and no client call happens.
	ResetCalls();
	Update();
	CHECK(g_clientCalls.empty());
	CHECK(g_handleCalls.empty());
	CHECK(g_serverCommands.empty());

	// The map starts: the gate opens and the first live update starts a recording.
	g_fake.currentMap = "de_dust2";
	FireMapStart();
	ResetCalls();
	Update();
	CHECK(CountServerCommands("tv_record ") == 1);
	CHECK(!g_clientCalls.empty());

	// A player_activate event reads userid through GetEventPlayerSlot, never GetEventInt.
	g_fake.eventSlot = 0;
	ResetCalls();
	CHECK(FireEvent("player_activate"));
	CHECK(Contains(g_eventSlotKeys, "userid"));
	CHECK(g_eventIntKeys.empty());

	// The chat/client-console command prints to the caller; the server console (-1) does not.
	const CommandRecord* command = FindCommand("demo");
	CHECK(command != nullptr);
	if (command != nullptr) {
		const plg::vector<plg::string> args{ToPlg("demo")};
		g_fake.clients[0].language = "portuguese";
		ResetCalls();
		const s2sdk::ResultType chat = command->callback(0, s2sdk::ConCommandContext::Chat, args);
		CHECK(chat == s2sdk::ResultType::Handled);
		CHECK(g_printed.size() == 1);
		CHECK(g_printed.front().second.find("PT") != std::string::npos);
		CHECK(g_printed.front().second.find("todos") != std::string::npos);

		ResetCalls();
		const s2sdk::ResultType server = command->callback(-1, s2sdk::ConCommandContext::Console, args);
		CHECK(server == s2sdk::ResultType::Continue);
		CHECK(g_printed.empty());
	}

	// OnMapEnd closes the gate; updates through the changelevel must not touch a client.
	FireMapEnd();
	ResetCalls();
	Update();
	CHECK(g_clientCalls.empty());
	CHECK(g_handleCalls.empty());
	// An event during the changelevel is inert too.
	ResetCalls();
	CHECK(FireEvent("round_start"));
	CHECK(g_clientCalls.empty());

	// OnPluginEnd removes every registration symmetrically and joins the services.
	CHECK(Plugify_PluginEnd().GetCode() == plg::PluginCode::Ok);
	CHECK(!g_mapStartRegistered);
	CHECK(!g_mapEndRegistered);
	CHECK(g_mapStartUnregistered);
	CHECK(g_mapEndUnregistered);
	CHECK(g_events.empty());
	CHECK(g_commands.empty());
	CHECK(Contains(g_commandRemoves, "demo"));
	CHECK(g_unhookCalls.size() == 3);
	ClearBindings();
}

void TestMidMapReloadAndNegativeHandle() {
	ResetRuntime();
	InstallBindings();
	// A plugin loaded into a running map: no OnMapStart will ever arrive.
	g_fake.currentMap = "de_dust2";
	g_fake.clients[0].connected = true;
	g_fake.clients[0].inGame = true;
	g_fake.clients[0].controller = true;
	g_fake.clients[0].handle = -2; // serial bit 31 set: a valid high-serial handle
	CHECK(Plugify_PluginStart().GetCode() == plg::PluginCode::Ok);
	Update();
	CHECK(CountServerCommands("tv_record ") == 1);
	CHECK(!g_handleCalls.empty());
	CHECK(Plugify_PluginEnd().GetCode() == plg::PluginCode::Ok);

	// A controller with the invalid handle -1 is not a player and cannot start a recording.
	ResetRuntime();
	InstallBindings();
	g_fake.currentMap = "de_mirage";
	g_fake.clients[0].connected = true;
	g_fake.clients[0].inGame = true;
	g_fake.clients[0].controller = true;
	g_fake.clients[0].handle = -1;
	CHECK(Plugify_PluginStart().GetCode() == plg::PluginCode::Ok);
	Update();
	CHECK(g_serverCommands.empty());
	CHECK(Plugify_PluginEnd().GetCode() == plg::PluginCode::Ok);
	ClearBindings();
}

void TestMissingMapBindingsStaysInactive() {
	ResetRuntime();
	InstallBindings();
	__s2sdk_OnMapStart_Register = nullptr; // the complete pair is required before registration
	__s2sdk_OnMapStart_Unregister = nullptr;
	g_fake.currentMap = "de_dust2";
	g_fake.clients[0].connected = true;
	g_fake.clients[0].inGame = true;
	CHECK(Plugify_PluginStart().GetCode() == plg::PluginCode::Ok);
	CHECK(!g_mapStartRegistered);
	CHECK(Contains(g_errors, std::string("The s2sdk map listener pair is unavailable - the plugin stays inactive."))
			  || !g_errors.empty());
	ResetCalls();
	Update();
	// Without listeners the mid-map latch must never open: no client and no recording call.
	CHECK(g_clientCalls.empty());
	CHECK(g_serverCommands.empty());
	CHECK(Plugify_PluginEnd().GetCode() == plg::PluginCode::Ok);
	ClearBindings();
}

void TestPreexistingCommandRemoval() {
	// AddConsoleCommand returns false when the name already exists: the plugin then only detaches
	// its listener and must never RemoveCommand a command it does not own (C11/A7).
	ResetRuntime();
	InstallBindings();
	g_commandOwned = false;
	g_fake.currentMap = "<empty>";
	CHECK(Plugify_PluginStart().GetCode() == plg::PluginCode::Ok);
	CHECK(g_commands.size() == 1);
	CHECK(g_commandAdds.front().callback != nullptr);
	CHECK(Plugify_PluginEnd().GetCode() == plg::PluginCode::Ok);
	CHECK(g_commands.empty());
	CHECK(Contains(g_listenerRemoves, "demo"));
	CHECK(g_commandRemoves.empty());
	ClearBindings();
}

void TestReloadWithMapBindings() {
	// A second start/end cycle after a full teardown: worker and plgdb must be restartable.
	ResetRuntime();
	InstallBindings();
	g_fake.currentMap = "de_dust2";
	g_fake.clients[0].connected = true;
	g_fake.clients[0].inGame = true;
	CHECK(Plugify_PluginStart().GetCode() == plg::PluginCode::Ok);
	Update();
	CHECK(CountServerCommands("tv_record ") == 1);
	CHECK(Plugify_PluginEnd().GetCode() == plg::PluginCode::Ok);

	ResetRuntime();
	InstallBindings();
	g_fake.currentMap = "de_inferno";
	g_fake.clients[0].connected = true;
	g_fake.clients[0].inGame = true;
	CHECK(Plugify_PluginStart().GetCode() == plg::PluginCode::Ok);
	Update();
	CHECK(CountServerCommands("tv_record ") == 1);
	CHECK(Plugify_PluginEnd().GetCode() == plg::PluginCode::Ok);
	ClearBindings();
}

std::string ExtractRecordedPath() {
	for (const std::string& command : g_serverCommands) {
		if (command.rfind("tv_record ", 0) != 0) {
			continue;
		}
		const std::size_t first = command.find('"');
		const std::size_t last = command.rfind('"');
		if (first != std::string::npos && last != std::string::npos && last > first) {
			return command.substr(first + 1, last - first - 1);
		}
	}
	return {};
}

void TestDemoProcessingAfterMapChange() {
	// A full recording -> stop at OnMapEnd -> 2 s update-driven delay on the next map -> zip ->
	// "processed" -> local cleanup. No s2sdk timer is involved at any point (C9/B4).
	ResetRuntime();
	InstallBindings();
	g_fake.currentMap = "<empty>";
	g_fake.clients[0].connected = true;
	g_fake.clients[0].inGame = true;
	g_fake.clients[0].controller = true;
	CHECK(Plugify_PluginStart().GetCode() == plg::PluginCode::Ok);
	Update(); // closes the one-shot latch with "<empty>"

	g_fake.currentMap = "de_dust2";
	FireMapStart();
	Update(); // starts the recording
	const std::string demoPath = ExtractRecordedPath();
	CHECK(!demoPath.empty());
	CHECK(demoPath.find("/demos/") != std::string::npos);
	if (demoPath.empty()) {
		CHECK(Plugify_PluginEnd().GetCode() == plg::PluginCode::Ok);
		ClearBindings();
		return;
	}
	WriteFile(demoPath, "demodata");
	// Request the demo, so DemoRequest.DeleteUnused keeps it (the production default).
	const CommandRecord* command = FindCommand("demo");
	CHECK(command != nullptr);
	if (command != nullptr) {
		const plg::vector<plg::string> args{ToPlg("demo")};
		CHECK(command->callback(0, s2sdk::ConCommandContext::Chat, args) == s2sdk::ResultType::Handled);
	}
	// Let the engine clock advance, refresh the cached time, then end the map.
	g_fake.gameTime += 10.0f;
	Update();
	FireMapEnd();
	const std::string zipPath = demoPath.substr(0, demoPath.size() - 4) + ".zip";
	CHECK(!std::filesystem::exists(zipPath));

	// The next map opens the gate; the pending demo is processed only after the 2 s delay.
	g_fake.currentMap = "de_nuke";
	FireMapStart();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	bool processed = false;
	while (std::chrono::steady_clock::now() < deadline && !processed) {
		Update(std::chrono::milliseconds(100));
		processed = std::any_of(g_infos.begin(), g_infos.end(), [](const std::string& line) {
			return line.find("Demo processed (no Discord webhook)") != std::string::npos;
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	CHECK(processed);
	CHECK(!ExtractRecordedPath().empty());

	// The completion deleted the demo and the zip (the shipped config deletes both).
	const auto deleteDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (std::chrono::steady_clock::now() < deleteDeadline
		   && (std::filesystem::exists(demoPath) || std::filesystem::exists(zipPath))) {
		Update(std::chrono::milliseconds(100));
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	CHECK(!std::filesystem::exists(demoPath));
	CHECK(!std::filesystem::exists(zipPath));

	CHECK(Plugify_PluginEnd().GetCode() == plg::PluginCode::Ok);
	ClearBindings();
}

void TestDatabasePathPriority() {
	// The fixture's databases.cfg is in <configs>/k4_gotv/databases.cfg and names a relative SQLite
	// path; the loader must use it (no "SQLite fallback" message) and create it under
	// <data>/k4_gotv/.
	ResetRuntime();
	InstallBindings();
	g_fake.currentMap = "<empty>";
	CHECK(Plugify_PluginStart().GetCode() == plg::PluginCode::Ok);
	// Let the plgdb worker create the schema file, then deliver its callback from an update.
	const std::filesystem::path expected = std::filesystem::path(g_dataDir) / "k4_gotv" / "db" / "custom.sqlite3";
	for (int attempt = 0; attempt < 50 && !std::filesystem::exists(expected); ++attempt) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		Update();
	}
	CHECK(g_errors.empty());
	CHECK(std::any_of(g_infos.begin(), g_infos.end(), [](const std::string& line) {
		return line.find("SQLite from databases.cfg") != std::string::npos;
	}));
	CHECK(!std::any_of(g_infos.begin(), g_infos.end(), [](const std::string& line) {
		return line.find("No 'k4_gotv' section") != std::string::npos;
	}));
	CHECK(std::filesystem::exists(expected));
	CHECK(Plugify_PluginEnd().GetCode() == plg::PluginCode::Ok);
	ClearBindings();
}

} // namespace

int main() {
	const std::string unique = std::to_string(static_cast<long long>(::getpid())) + "_"
							   + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
	const std::filesystem::path root = std::filesystem::temp_directory_path() / ("k4_gotv_runtime_" + unique);
	g_configsDir = (root / "configs").string();
	g_dataDir = (root / "data").string();
	CreateFixture();
	ResetRuntime();

	TestStartBeforeMapGate();
	TestMidMapReloadAndNegativeHandle();
	TestMissingMapBindingsStaysInactive();
	TestPreexistingCommandRemoval();
	TestDemoProcessingAfterMapChange();
	TestReloadWithMapBindings();
	TestDatabasePathPriority();

	RemoveFixture();
	std::printf("k4_gotv_runtime_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
