// k4_gotv — a minimal KeyValues 1 reader for the plugin's `databases.cfg`.
//
// pisexcompat used to provide this; with the plugin self-contained on s2sdk + plgdb the only
// KeyValues file left is `<configs>/k4_gotv/databases.cfg` (PLUGIN_STANDARD.md section 4). The
// reader keeps the wrapper's observable behaviour that the loader depends on:
//   * `"Root" { ... }` is collapsed, so the root node holds the root block's children (a file with
//     a single "Databases" block exposes its sections at the root);
//   * key lookup is case-insensitive;
//   * `//` comments, quoted and unquoted tokens and nested blocks are accepted;
//   * a malformed file is reported (ParseKv1 returns false) and leaves no half-parsed tree behind.
//
// Engine-free: tests/k4_gotv_tests.cpp parses sample files with it.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace k4 {

struct Kv1Node {
	std::string name;
	std::string value;
	bool hasValue = false;
	std::vector<Kv1Node> children;

	// Direct child by name, case-insensitive; nullptr when absent.
	const Kv1Node* Find(std::string_view key) const;
	// Value of a direct child, or `fallback` when the child is missing/has no value.
	std::string GetString(std::string_view key, std::string_view fallback) const;
	// Value of a direct child parsed as a base-10 integer; `fallback` when missing or non-numeric.
	int GetInt(std::string_view key, int fallback) const;
};

// Parses the text of a KeyValues 1 file. On failure `root` is left empty.
bool ParseKv1(std::string_view text, Kv1Node& root);

} // namespace k4
