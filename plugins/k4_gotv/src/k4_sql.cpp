// k4_gotv — the `k4-gotv` table statements (see k4_sql.hpp). Moved verbatim from
// k4_database.cpp; the INSERT gained the idempotence check.

#include "k4_sql.hpp"

#include <cstdint>

namespace k4 {

std::string QuoteTableName(std::string_view table, bool mysql) {
	return mysql ? std::string("`") + std::string(table) + "`" : std::string("\"") + std::string(table) + "\"";
}

std::string QuoteDemoTable(bool mysql) {
	return QuoteTableName(kDemoTableName, mysql);
}

std::string CreateDemoTableSql(bool mysql, std::string_view tableName) {
	const std::string table = QuoteTableName(tableName, mysql);
	if (mysql) {
		// The original's DDL, column for column (k4-GOTV 1.0.2, Plugin.Database.cs).
		return "CREATE TABLE IF NOT EXISTS " + table
			   + " ("
				 "id BIGINT AUTO_INCREMENT PRIMARY KEY,"
				 "map VARCHAR(255) NOT NULL,"
				 "date DATE NOT NULL,"
				 "time TIME NOT NULL,"
				 "length VARCHAR(8) NOT NULL,"
				 "round INT NOT NULL,"
				 "mega_link TEXT NOT NULL,"
				 "ftp_link TEXT NOT NULL,"
				 "requester_name TEXT NOT NULL,"
				 "requester_steamid TEXT NOT NULL,"
				 "requester_count INT NOT NULL,"
				 "player_count INT NOT NULL,"
				 "server_name VARCHAR(255) NOT NULL,"
				 "file_name VARCHAR(255) NOT NULL,"
				 "KEY idx_map_date (map, date)"
				 ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";
	}
	return "CREATE TABLE IF NOT EXISTS " + table
		   + " ("
			 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
			 "map TEXT NOT NULL,"
			 "date TEXT NOT NULL,"
			 "time TEXT NOT NULL,"
			 "length TEXT NOT NULL,"
			 "round INTEGER NOT NULL,"
			 "mega_link TEXT NOT NULL,"
			 "ftp_link TEXT NOT NULL,"
			 "requester_name TEXT NOT NULL,"
			 "requester_steamid TEXT NOT NULL,"
			 "requester_count INTEGER NOT NULL,"
			 "player_count INTEGER NOT NULL,"
			 "server_name TEXT NOT NULL,"
			 "file_name TEXT NOT NULL)";
}

std::string CreateDemoIndexSql(bool mysql, std::string_view tableName) {
	return std::string("CREATE INDEX IF NOT EXISTS idx_map_date ON ") + QuoteTableName(tableName, mysql) + " (map, date)";
}

std::string InsertDemoRowSql(bool mysql, std::string_view tableName) {
	const std::string table = QuoteTableName(tableName, mysql);
	std::string sql = "INSERT INTO " + table
					  + " (map, date, time, length, round, mega_link, ftp_link, requester_name, "
						"requester_steamid, requester_count, player_count, server_name, file_name) "
						"SELECT ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?";
	if (mysql) {
		sql += " FROM DUAL"; // MySQL needs a FROM for the WHERE without a source table.
	}
	sql += " WHERE NOT EXISTS (SELECT 1 FROM " + table
		   + " WHERE file_name = ? AND date = ? AND time = ?)";
	return sql;
}

plgdb::Params DemoInsertParams(const DemoRecord& record) {
	plgdb::Params params;
	params.reserve(kDemoInsertParameterCount);
	params.emplace_back(record.map);
	params.emplace_back(record.date);
	params.emplace_back(record.time);
	params.emplace_back(record.length);
	params.emplace_back(static_cast<int64_t>(record.round));
	params.emplace_back(record.megaLink);
	params.emplace_back(record.ftpLink);
	params.emplace_back(record.requesterName);
	params.emplace_back(record.requesterSteamId);
	params.emplace_back(static_cast<int64_t>(record.requesterCount));
	params.emplace_back(static_cast<int64_t>(record.playerCount));
	params.emplace_back(record.serverName);
	params.emplace_back(record.fileName);
	// The idempotence check: the same file_name/date/time values as in the SELECT above.
	params.emplace_back(record.fileName);
	params.emplace_back(record.date);
	params.emplace_back(record.time);
	return params;
}

} // namespace k4
