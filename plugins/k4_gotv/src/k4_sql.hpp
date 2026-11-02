// k4_gotv — the `k4-gotv` table statements (engine-free: used by k4_database.cpp and executed by
// the SQLite test). The DDL and the INSERT live here so the tests can run exactly the statements
// the plugin sends against a real SQLite database.
//
// The INSERT is idempotent: plgdb refuses to resend a statement whose outcome is unknown (the
// connection dropped after the server may have applied it) and reports it as an error, so the
// plugin's bounded retry could otherwise store the same demo twice. The table has no unique key
// (the original's schema is kept column for column), so the statement inserts only when no row
// with the same file_name/date/time exists.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <plgdb/db.hpp>

namespace k4 {

constexpr const char* kDemoTableName = "k4-gotv";
// Placeholders expected by InsertDemoRowSql(): the 13 column values plus file_name, date and time
// of the idempotence check.
constexpr std::size_t kDemoInsertParameterCount = 16;

struct DemoRecord {
	std::string map;
	std::string date;
	std::string time;
	std::string length;
	int round = 1;
	std::string megaLink;
	std::string ftpLink;
	std::string requesterName;
	std::string requesterSteamId;
	int requesterCount = 0;
	int playerCount = 0;
	std::string serverName;
	std::string fileName;
};

// MySQL quotes identifiers with backticks, SQLite with double quotes. `table` must already be a
// trusted identifier (the plugin passes the constant name; the MySQL test passes a random
// alphanumeric name).
std::string QuoteTableName(std::string_view table, bool mysql);
std::string QuoteDemoTable(bool mysql);
std::string CreateDemoTableSql(bool mysql, std::string_view table = kDemoTableName);
std::string CreateDemoIndexSql(bool mysql, std::string_view table = kDemoTableName);
// INSERT ... SELECT ... FROM DUAL (MySQL) / without FROM (SQLite) ... WHERE NOT EXISTS
// (file_name, date, time).
std::string InsertDemoRowSql(bool mysql, std::string_view table = kDemoTableName);
// The 13 column values followed by the three identity values, in InsertDemoRowSql() order.
plgdb::Params DemoInsertParams(const DemoRecord& record);

} // namespace k4
