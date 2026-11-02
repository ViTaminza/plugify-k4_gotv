// k4_gotv — demo metadata logging through plgdb (MySQL, SQLite fallback).
//
// The original wrote a row into `k4-gotv` through Dapper (huge columns and all): map, date, time,
// length, round, mega_link, ftp_link, requester_name, requester_steamid, requester_count,
// player_count, server_name, file_name. The same table and columns are kept here. The user's
// decision puts the S3 link where the original put the external links: the object URL goes into
// mega_link and the object key into ftp_link. PostgreSQL is dropped (plgdb supports MySQL and
// SQLite only).
//
// Connection settings come from <configs>/k4_gotv/databases.cfg, section "k4_gotv" (the Pisex KV
// format, like every other ported plugin); without that section a local SQLite file is used, so
// the plugin keeps working on a test server. plgdb runs every statement on its own worker;
// Poll() (OnPluginUpdate) delivers the callbacks on the game thread.

#pragma once

#include <memory>
#include <string>

#include <plgdb/db.hpp>

#include "k4_sql.hpp" // DemoRecord and the table statements

namespace k4 {

class DatabaseService {
public:
	DatabaseService();
	~DatabaseService();
	DatabaseService(const DatabaseService&) = delete;
	DatabaseService& operator=(const DatabaseService&) = delete;

	// Creates the connection and queues CREATE TABLE IF NOT EXISTS (re-run before an INSERT until
	// it succeeded, so a startup while the database is down recovers on its own). Returns false
	// when the config is invalid (the service then stays disabled).
	// `configSourceFallback` is true when no databases.cfg section was found (a local SQLite file);
	// it only changes the log line, never the connection.
	bool Configure(plgdb::Config config, bool configSourceFallback = true);
	bool Enabled() const;

	// Queues the INSERT; a failed write is retried a few times from Poll() (bounded queue).
	void Store(const DemoRecord& record);
	void Poll();
	// Flushes queued statements (plgdb's destructor joins the worker), then drops the connection.
	void Shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

// Builds the plgdb config from <configs>/k4_gotv/databases.cfg (section "k4_gotv"; keys host,
// port, user, pass, database, optional driver="sqlite" and sqlite_path). When the section is
// missing, returns a SQLite configuration at <data dir>/k4_gotv/k4_gotv.sqlite3 and sets
// *usingFallback.
plgdb::Config LoadDatabaseConfig(bool* usingFallback);

} // namespace k4
