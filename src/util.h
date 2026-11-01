#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fsrv {

// ---- identifiers / time -----------------------------------------------
std::string uuid_v4();
int64_t now_sec();
int64_t now_ms();
std::string format_time_iso(int64_t unix_sec);

// ---- strings ----------------------------------------------------------
// RFC 3986 percent decoding; '+' is left untouched. Returns false on a
// malformed escape sequence.
bool percent_decode(std::string_view in, std::string& out);
std::string ascii_lower(std::string_view in);
bool ascii_iequals(std::string_view a, std::string_view b);
bool ascii_istarts_with(std::string_view s, std::string_view prefix);
std::string trim(std::string_view in);
bool is_valid_utf8(std::string_view in);

// ---- logging ----------------------------------------------------------
void log_info(const std::string& msg);
void log_warn(const std::string& msg);
void log_error(const std::string& msg);

// ---- misc -------------------------------------------------------------
std::string human_size(uint64_t bytes);

}  // namespace fsrv
