// k4_gotv — small text helpers that used to come from pisexcompat: the Source chat colour
// tags, the client-language normalization and the log redaction of credentials.
//
// Engine-free (no plg/s2sdk header) so tests/k4_gotv_tests.cpp can check the exact byte output of
// the colour tags and the redaction rules.

#pragma once

#include <string>
#include <string_view>

namespace k4 {

// Pisex's Colorize table {TAG -> byte}: cs2-menus/utils.h colors_text[]/colors_hex[], same order,
// same bytes. The last entry is the chat line break: the literal "\n" -> U+2029 (e2 80 a9),
// which the CS2 chat renderer shows as a new line.
std::string Colorize(std::string_view text);
// Same table, but every colour tag becomes an empty string and the literal "\n" a real newline.
// Used for plain-text log lines.
std::string StripColorTags(std::string_view text);

// Source language names -> the codes used by the Pisex phrase files (the mapping pisexcompat
// used): "english"/"en" -> "en", "portuguese"/"pt-br"/"brazilian" -> "pt-BR", and so on. An
// unknown value is returned unchanged, exactly like the wrapper.
std::string NormalizeLanguage(std::string_view name);

// Keeps scheme + host of a URL and replaces the path with "/...". A Discord webhook URL is a
// credential (its token grants post/delete access), and an S3 object key identifies private
// data, so neither may ever reach the log. A value without "://" becomes "...".
std::string RedactUrl(std::string_view url);

// Defensive redaction for an error string that may quote a request: masks AWS SigV4 query
// credentials/signatures and password/token query values, without touching an ordinary URL path.
// Log lines that print a URL must use RedactUrl() as well.
std::string RedactSensitive(std::string text);

} // namespace k4
