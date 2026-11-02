// Exercise the `k4-gotv` table statements against a real in-memory SQLite database and, when
// ./test.sh exports MYSQL_HOST/MYSQL_PORT/MYSQL_USER/MYSQL_PASSWORD/MYSQL_DATABASE, against the
// throwaway MariaDB too. The schema, the placeholder/parameter match and - the point of the
// idempotent INSERT - a retried statement must not insert the same demo twice.
//
// The MySQL leg creates a random, test-only table (`k4_gotv_test_<pid>_<random>`) so it can never
// touch the production `k4-gotv` table, and drops it again. Credentials are never printed.
#include "k4_sql.hpp"

#include <plgdb/db.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <variant>

#include <unistd.h>

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
	if (!ok) {
		++failures;
		std::cerr << "SQL test failed: " << message << '\n';
	}
}

std::size_t CountPlaceholders(const std::string& sql) {
	std::size_t count = 0;
	for (const char character : sql) {
		if (character == '?') {
			++count;
		}
	}
	return count;
}

k4::DemoRecord MakeRecord() {
	k4::DemoRecord record;
	record.map = "de_dust2";
	record.date = "2026-10-24";
	record.time = "12:34:56";
	record.length = "05:07";
	record.round = 7;
	record.megaLink = "https://s3.example/bucket/demos/a.zip";
	record.ftpLink = "demos/a.zip";
	record.requesterName = "Player";
	record.requesterSteamId = "76561198000000000";
	record.requesterCount = 1;
	record.playerCount = 32;
	record.serverName = "Test Server";
	record.fileName = "autodemo_de_dust2_2026-10-24_12-34-56";
	return record;
}

// A random table name that is safe as an identifier without quoting tricks.
std::string NewMysqlTableName() {
	std::random_device device;
	const uint64_t random = (static_cast<uint64_t>(device()) << 32) ^ static_cast<uint64_t>(
								 std::chrono::steady_clock::now().time_since_epoch().count());
	static constexpr char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
	std::string suffix;
	uint64_t value = random;
	for (int index = 0; index < 12; ++index) {
		suffix.push_back(kAlphabet[value % (sizeof(kAlphabet) - 1)]);
		value /= sizeof(kAlphabet) - 1;
	}
	return "k4_gotv_test_" + std::to_string(static_cast<long long>(::getpid())) + "_" + suffix;
}

void RunMysqlLeg(const char* host) {
	plgdb::Config config;
	config.driver = plgdb::Driver::MySQL;
	config.host = host;
	const char* port = std::getenv("MYSQL_PORT");
	config.port = static_cast<uint16_t>(std::atoi(port != nullptr && *port != '\0' ? port : "3306"));
	const char* user = std::getenv("MYSQL_USER");
	const char* password = std::getenv("MYSQL_PASSWORD");
	const char* database = std::getenv("MYSQL_DATABASE");
	config.user = user != nullptr ? user : "";
	config.password = password != nullptr ? password : "";
	config.database = database != nullptr && *database != '\0' ? database : "plgtest";
	config.charset = "utf8mb4";

	const std::string table = NewMysqlTableName();
	auto databaseHandle = plgdb::Database::Create(config);
	Check(databaseHandle != nullptr, "MySQL database handle");
	if (databaseHandle == nullptr) {
		return;
	}

	const plgdb::Result create = databaseHandle->QuerySync(k4::CreateDemoTableSql(true, table), {});
	Check(create.Ok(), "MySQL CREATE TABLE (random test table)");
	if (!create.Ok()) {
		std::cerr << "MySQL CREATE TABLE failed: " << create.Error() << '\n';
		return;
	}

	const k4::DemoRecord record = MakeRecord();
	const plgdb::Params params = k4::DemoInsertParams(record);
	Check(databaseHandle->QuerySync(k4::InsertDemoRowSql(true, table), params).Ok(), "MySQL first INSERT");
	Check(databaseHandle->QuerySync(k4::InsertDemoRowSql(true, table), params).Ok(), "MySQL retried INSERT");
	const plgdb::Result count = databaseHandle->QuerySync("SELECT COUNT(*) FROM `" + table + "`", {});
	Check(count.Ok() && count.RowCount() == 1 && count.ColumnCount() == 1,
		  "MySQL retried INSERT does not duplicate the row");
	if (count.Ok() && count.RowCount() == 1 && count.ColumnCount() == 1) {
		Check(std::get<int64_t>(count.At(0, 0)) == 1, "one row in the MySQL table");
	}

	k4::DemoRecord second = record;
	second.fileName = "autodemo_de_dust2_2026-10-24_12-40-00";
	second.time = "12:40:00";
	Check(databaseHandle->QuerySync(k4::InsertDemoRowSql(true, table), k4::DemoInsertParams(second)).Ok(),
		  "MySQL second demo INSERT");
	const plgdb::Result countAgain = databaseHandle->QuerySync("SELECT COUNT(*) FROM `" + table + "`", {});
	Check(countAgain.Ok() && countAgain.RowCount() == 1 && countAgain.ColumnCount() == 1
			  && std::get<int64_t>(countAgain.At(0, 0)) == 2,
		  "a different demo is inserted in MySQL");

	const plgdb::Result drop = databaseHandle->QuerySync("DROP TABLE IF EXISTS `" + table + "`", {});
	Check(drop.Ok(), "MySQL DROP TABLE (cleanup)");
	std::cout << "MySQL leg ran on a random table; it was dropped again." << '\n';
}
} // namespace

int RunSqlTests() {
	k4::DemoRecord record = MakeRecord();

	const plgdb::Params params = k4::DemoInsertParams(record);
	Check(params.size() == k4::kDemoInsertParameterCount, "parameter count");
	for (const bool mysql : {true, false}) {
		const std::string sql = k4::InsertDemoRowSql(mysql);
		Check(CountPlaceholders(sql) == params.size(), "placeholder count matches the bound parameters");
		Check(sql.find("WHERE NOT EXISTS") != std::string::npos, "INSERT is idempotent");
		Check((sql.find("FROM DUAL") != std::string::npos) == mysql, "FROM DUAL only for MySQL");
		Check(sql.find(k4::kDemoTableName) != std::string::npos, "statement names the demo table");
	}
	Check(k4::QuoteTableName("t_test", true) == "`t_test`", "MySQL identifier quoting");
	Check(k4::QuoteTableName("t_test", false) == "\"t_test\"", "SQLite identifier quoting");

	plgdb::Config config;
	config.driver = plgdb::Driver::SQLite;
	config.sqlitePath = ":memory:";
	auto database = plgdb::Database::Create(config);
	Check(database != nullptr, "SQLite database");
	if (database == nullptr) {
		std::cout << "SQL test failures: " << failures << '\n';
		return failures;
	}
	Check(database->QuerySync(k4::CreateDemoTableSql(false), {}).Ok(), "CREATE TABLE");
	Check(database->QuerySync(k4::CreateDemoIndexSql(false), {}).Ok(), "CREATE INDEX");
	Check(database->QuerySync(k4::InsertDemoRowSql(false), params).Ok(), "first INSERT");
	// The retry after a lost response must not add the demo twice.
	Check(database->QuerySync(k4::InsertDemoRowSql(false), params).Ok(), "retried INSERT succeeds");
	const plgdb::Result count = database->QuerySync("SELECT COUNT(*) FROM \"k4-gotv\"", {});
	Check(count.Ok() && count.RowCount() == 1, "a retried INSERT does not duplicate the row");
	if (count.Ok() && count.RowCount() == 1 && count.ColumnCount() == 1) {
		Check(std::get<int64_t>(count.At(0, 0)) == 1, "one row in the table");
	}

	// A different demo (other file_name/date/time) is still inserted.
	record.fileName = "autodemo_de_dust2_2026-10-24_12-40-00";
	record.time = "12:40:00";
	Check(database->QuerySync(k4::InsertDemoRowSql(false), k4::DemoInsertParams(record)).Ok(),
		  "second demo INSERT");
	const plgdb::Result countAgain = database->QuerySync("SELECT COUNT(*) FROM \"k4-gotv\"", {});
	Check(countAgain.Ok() && countAgain.RowCount() == 1 && countAgain.ColumnCount() == 1
			  && std::get<int64_t>(countAgain.At(0, 0)) == 2,
		  "a different demo is inserted");

	// ./test.sh exports the throwaway MariaDB; the same statements then run on the driver the
	// production database uses. A random table keeps the production `k4-gotv` untouched.
	const char* host = std::getenv("MYSQL_HOST");
	if (host != nullptr && *host != '\0') {
		RunMysqlLeg(host);
	} else {
		std::cout << "MySQL leg skipped (MYSQL_HOST is not set)." << '\n';
	}

	std::cout << "SQL test failures: " << failures << '\n';
	return failures;
}
