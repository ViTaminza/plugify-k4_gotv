// k4_gotv — the demo metadata table through plgdb (see k4_database.hpp).

#include "k4_database.hpp"

#include "k4_kv1.hpp"
#include "k4_text.hpp"

#include <plg/plugin.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace k4 {
namespace {

constexpr const char* kLogTag = "k4_gotv:";
constexpr const char* kDatabaseSection = "k4_gotv";
constexpr const char* kPluginDir = "k4_gotv";
constexpr const char* kDefaultDatabase = "k4_gotv";
constexpr const char* kDefaultSqliteFile = "k4_gotv.sqlite3";
// The spelling the Metamod original read; libs/pisexcompat resolved it to the plugin's own
// config directory (see DatabasesCfgCandidates below).
constexpr const char* kMetamodDatabasesCfgPath = "addons/configs/databases.cfg";
// A transient MySQL outage when a demo finishes must not lose its metadata row: a failed INSERT
// is retried a few times (15 s apart) instead of being dropped after one attempt.
constexpr int kMaximumInsertAttempts = 4; // the first insert plus three retries
constexpr std::size_t kMaximumRetryQueue = 100;
constexpr auto kRetrySpacing = std::chrono::seconds(15);

void LogInfo(const std::string& message) {
	if (plg::Log) {
		plg::Log(std::string(kLogTag) + " " + RedactSensitive(message), plg::Severity::Info,
				 plg::source_location::current());
	}
}

void LogWarning(const std::string& message) {
	if (plg::Log) {
		plg::Log(std::string(kLogTag) + " " + RedactSensitive(message), plg::Severity::Warning,
				 plg::source_location::current());
	}
}

void LogError(const std::string& message) {
	if (plg::Log) {
		plg::Log(std::string(kLogTag) + " " + RedactSensitive(message), plg::Severity::Error,
				 plg::source_location::current());
	}
}

// --- paths (replacing pisex::JoinPath / ConfigDir / DataDir) ---------------------------------

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
	path.append(leaf);
	return path;
}

// <GetConfigsDir()>/<plugin>. With no plugify directory available this degrades to the relative
// "k4_gotv", exactly like pisexcompat's path helper did (the pre-standard candidate list).
std::string PluginConfigDir() {
	const std::string root = plg::GetConfigsDir ? std::string(plg::GetConfigsDir()) : std::string();
	return JoinPath(root, kPluginDir);
}

// <GetDataDir()>/<plugin>.
std::string PluginDataDir() {
	const std::string root = plg::GetDataDir ? std::string(plg::GetDataDir()) : std::string();
	return JoinPath(root, kPluginDir);
}

bool FileExists(std::string_view path) {
	if (path.empty()) {
		return false;
	}
	std::ifstream file(std::string(path), std::ios::binary);
	return file.good();
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

// The candidates pisexcompat's ResolvePath("addons/configs/databases.cfg") tried, in the same
// order: the path as given relative to the server working directory, then the plugin's own
// directories. <GetConfigsDir()> itself is deliberately not a candidate - a shared
// configs/databases.cfg belongs to other plugins.
std::vector<std::string> DatabasesCfgCandidates() {
	std::vector<std::string> candidates;
	candidates.emplace_back(kMetamodDatabasesCfgPath);
	const std::string configDir = PluginConfigDir();
	if (!configDir.empty()) {
		candidates.push_back(JoinPath(configDir, "databases.cfg"));
		candidates.push_back(JoinPath(JoinPath(configDir, "translations"), "databases.cfg"));
	}
	const std::string dataDir = PluginDataDir();
	if (!dataDir.empty()) {
		candidates.push_back(JoinPath(dataDir, "databases.cfg"));
	}
	return candidates;
}

// `sqlite_path` is relative to <data dir>/<plugin>, like pisex::ResolveDataPath.
std::string ResolveDataPath(std::string_view path) {
	if (path.empty()) {
		return {};
	}
	std::string file(path);
	constexpr std::string_view kDataPrefix = "addons/data/";
	if (file.rfind(kDataPrefix, 0) == 0) {
		file.erase(0, kDataPrefix.size());
	}
	return JoinPath(PluginDataDir(), file);
}

void CreateParentDirectory(const std::string& path) {
	if (path.empty()) {
		return;
	}
	const std::filesystem::path parent = std::filesystem::path(path).parent_path();
	if (parent.empty()) {
		return;
	}
	std::error_code error;
	std::filesystem::create_directories(parent, error);
}

bool EqualsNoCase(std::string_view left, std::string_view right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.size(); ++index) {
		if (std::tolower(static_cast<unsigned char>(left[index]))
			!= std::tolower(static_cast<unsigned char>(right[index]))) {
			return false;
		}
	}
	return true;
}

// The "k4_gotv" section of a parsed databases.cfg. ParseKv1 collapses a single top-level
// "Databases" block into the root node, so the section is a direct child; a hand-written file
// that consists of just the "k4_gotv" block is accepted too (the root carries its name).
const Kv1Node* FindSection(const Kv1Node& root) {
	if (const Kv1Node* section = root.Find(kDatabaseSection); section != nullptr && !section->hasValue) {
		return section;
	}
	if (!root.hasValue && EqualsNoCase(root.name, kDatabaseSection)) {
		return &root;
	}
	return nullptr;
}

} // namespace

plgdb::Config LoadDatabaseConfig(bool* usingFallback) {
	if (usingFallback != nullptr) {
		*usingFallback = false;
	}
	plgdb::Config config;
	config.driver = plgdb::Driver::MySQL;
	config.charset = "utf8mb4";

	bool configured = false;
	// The first candidate that exists on disk is authoritative, exactly like the pre-standard
	// reader: it was resolved once and then loaded. A broken file is reported below and the plugin
	// falls back to SQLite; it never silently switches to a different candidate.
	std::string databasesPath;
	for (const std::string& candidate : DatabasesCfgCandidates()) {
		if (FileExists(candidate)) {
			databasesPath = candidate;
			break;
		}
	}
	if (!databasesPath.empty()) {
		bool fileOk = false;
		const std::string text = ReadFile(databasesPath, &fileOk);
		Kv1Node root;
		if (fileOk && ParseKv1(text, root)) {
			if (const Kv1Node* entry = FindSection(root); entry != nullptr) {
				configured = true;
				config.host = entry->GetString("host", "127.0.0.1");
				config.port = static_cast<uint16_t>(std::max(1, std::min(65535, entry->GetInt("port", 3306))));
				config.user = entry->GetString("user", "");
				config.password = entry->GetString("pass", "");
				config.database = entry->GetString("database", kDefaultDatabase);
				if (entry->GetString("driver", "mysql") == "sqlite") {
					config.driver = plgdb::Driver::SQLite;
					config.sqlitePath = ResolveDataPath(entry->GetString("sqlite_path", kDefaultSqliteFile));
				}
			}
		}
	}

	if (!configured) {
		// Fail soft, like the other ported plugins: a local SQLite file keeps the metadata logging
		// working on a test server without a databases.cfg entry.
		if (usingFallback != nullptr) {
			*usingFallback = true;
		}
		config.driver = plgdb::Driver::SQLite;
		config.sqlitePath = JoinPath(PluginDataDir(), kDefaultSqliteFile);
	}
	if (config.driver == plgdb::Driver::SQLite) {
		CreateParentDirectory(config.sqlitePath);
	}
	return config;
}

struct DatabaseService::Impl {
	struct Retry {
		DemoRecord record;
		int attempts = 0;
	};

	std::unique_ptr<plgdb::Database> database;
	bool enabled = false;
	bool mysql = false;
	bool schemaReady = false;
	std::deque<Retry> retries;
	std::chrono::steady_clock::time_point nextRetry {};

	void CreateSchema();
	void Insert(const DemoRecord& record, int attempts);
};

void DatabaseService::Impl::CreateSchema() {
	database->Query(CreateDemoTableSql(mysql), {}, [this](const plgdb::Result& result) {
		if (!result.Ok()) {
			LogError("Database schema creation failed: " + result.Error());
			return;
		}
		schemaReady = true;
		if (!mysql) {
			database->Query(CreateDemoIndexSql(mysql), {}, [](const plgdb::Result& indexResult) {
				if (!indexResult.Ok()) {
					LogError("Database index creation failed: " + indexResult.Error());
				}
			});
		}
	});
}

void DatabaseService::Impl::Insert(const DemoRecord& record, int attempts) {
	// A fresh deployment can start while MySQL is still down: the startup CREATE TABLE then
	// failed and every INSERT would fail with "table doesn't exist". Re-run the (IF NOT EXISTS)
	// DDL before inserting until one creation succeeded.
	if (!schemaReady) {
		CreateSchema();
	}

	// Idempotent (INSERT ... WHERE NOT EXISTS): the retry below must not duplicate a row whose
	// first INSERT was applied by the server but reported as failed (plgdb will not resend an
	// uncertain statement itself, see InsertDemoRowSql).
	database->Query(InsertDemoRowSql(mysql), DemoInsertParams(record),
					[this, record, attempts](const plgdb::Result& result) {
						if (result.Ok()) {
							return;
						}
						if (attempts + 1 >= kMaximumInsertAttempts || retries.size() >= kMaximumRetryQueue) {
							LogError("Database write failed: " + result.Error() + "; the row is not stored.");
							return;
						}
						LogWarning("Database write failed (attempt " + std::to_string(attempts + 1) + " of "
								   + std::to_string(kMaximumInsertAttempts) + "): " + result.Error()
								   + "; retrying.");
						retries.push_back({record, attempts + 1});
						nextRetry = std::chrono::steady_clock::now() + kRetrySpacing;
					});
}

DatabaseService::DatabaseService() : m_impl(std::make_unique<Impl>()) {}

DatabaseService::~DatabaseService() {
	Shutdown();
}

bool DatabaseService::Configure(plgdb::Config config, bool configSourceFallback) {
	if (m_impl->database != nullptr) {
		m_impl->database.reset(); // a reconfigure must not leave the previous worker behind
	}
	m_impl->retries.clear();
	m_impl->schemaReady = false;
	m_impl->database = plgdb::Database::Create(config);
	if (m_impl->database == nullptr) {
		LogError("The database configuration is invalid; demo metadata will not be stored.");
		m_impl->enabled = false;
		return false;
	}
	m_impl->mysql = m_impl->database->GetDriver() == plgdb::Driver::MySQL;
	m_impl->enabled = true;
	m_impl->schemaReady = false;
	m_impl->CreateSchema();
	if (m_impl->mysql) {
		LogInfo("Database logging enabled (MySQL, table " + std::string(kDemoTableName) + ").");
	} else if (configSourceFallback) {
		LogInfo("Database logging enabled (SQLite fallback, table " + std::string(kDemoTableName) + ").");
	} else {
		LogInfo("Database logging enabled (SQLite from databases.cfg, table " + std::string(kDemoTableName) + ").");
	}
	return true;
}

bool DatabaseService::Enabled() const {
	return m_impl->enabled && m_impl->database != nullptr;
}

void DatabaseService::Store(const DemoRecord& record) {
	if (!Enabled()) {
		return;
	}
	m_impl->Insert(record, 0);
}

void DatabaseService::Poll() {
	Impl& impl = *m_impl;
	if (impl.database != nullptr) {
		impl.database->Poll();
	}
	const auto now = std::chrono::steady_clock::now();
	if (impl.database != nullptr && !impl.retries.empty() && now >= impl.nextRetry) {
		Impl::Retry retry = std::move(impl.retries.front());
		impl.retries.pop_front();
		impl.nextRetry = now + kRetrySpacing;
		impl.Insert(retry.record, retry.attempts);
	}
}

void DatabaseService::Shutdown() {
	if (m_impl->database != nullptr) {
		m_impl->database.reset(); // plgdb flushes queued statements and joins the worker
	}
	m_impl->retries.clear();
	m_impl->enabled = false;
}

} // namespace k4
