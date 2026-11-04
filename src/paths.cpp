#include "paths.h"

#include <algorithm>
#include <vector>

#include "error.h"
#include "util.h"

namespace fsrv {

namespace {

constexpr size_t kMaxSegmentBytes = 255;
constexpr size_t kMaxPathBytes = 32000;

bool is_reserved_base(const std::string& base) {
  return ascii_iequals(base, "CON") || ascii_iequals(base, "PRN") ||
         ascii_iequals(base, "AUX") || ascii_iequals(base, "NUL") ||
         (base.size() == 4 &&
          (ascii_istarts_with(base, "COM") || ascii_istarts_with(base, "LPT")) &&
          base[3] >= '1' && base[3] <= '9');
}

bool is_forbidden_char(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  if (u < 0x20 || u == 0x7F) return true;
  switch (c) {
    case '<':
    case '>':
    case ':':
    case '"':
    case '|':
    case '?':
    case '*':
      return true;
    default:
      return false;
  }
}

bool rel_has_prefix(std::string_view rel, std::string_view prefix) {
  if (rel.size() < prefix.size()) return false;
  const std::string_view head = rel.substr(0, prefix.size());
#if defined(_WIN32)
  if (!ascii_iequals(head, prefix)) return false;
#else
  if (head != prefix) return false;
#endif
  return rel.size() == prefix.size() || rel[prefix.size()] == '/';
}

}  // namespace

bool is_windows_reserved_name(std::string_view name) {
  std::string s(name);
  const size_t dot = s.find('.');
  const std::string base = dot == std::string::npos ? s : s.substr(0, dot);
  return is_reserved_base(base);
}

bool PathGuard::is_within(const fs::path& child, const fs::path& parent) {
  auto ci = child.begin();
  auto pi = parent.begin();
  for (; pi != parent.end(); ++pi, ++ci) {
    if (ci == child.end()) return false;
    const std::string a = ci->u8string();
    const std::string b = pi->u8string();
#if defined(_WIN32)
    if (!ascii_iequals(a, b)) return false;
#else
    if (a != b) return false;
#endif
  }
  return true;
}

PathGuard PathGuard::make(const fs::path& root,
                          std::vector<fs::path> reserved) {
  std::error_code ec;
  if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
    throw internal_error("root directory does not exist: " + root.u8string(),
                         "ROOT_MISSING");
  }
  PathGuard tmp;
  tmp.root_ = fs::absolute(root).lexically_normal();
  tmp.root_canon_ = fs::weakly_canonical(tmp.root_, ec);
  if (ec) tmp.root_canon_ = tmp.root_;

  for (const fs::path& entry : reserved) {
    fs::path abs = entry.is_absolute() ? entry : (tmp.root_ / entry);
    abs = abs.lexically_normal();
    if (!is_within(abs, tmp.root_)) continue;

    fs::path rel = abs.lexically_relative(tmp.root_);
    if (!rel.empty() && rel.generic_u8string() != ".") {
      tmp.reserved_rel_.push_back(rel.generic_u8string());
    }

    std::error_code canon_ec;
    fs::path canon = fs::weakly_canonical(abs, canon_ec);
    if (canon_ec) canon = abs;
    tmp.reserved_canon_.push_back(std::move(canon));
  }
  return tmp;
}

void PathGuard::reject_reserved_rel(std::string_view rel) const {
  for (const std::string& reserved : reserved_rel_) {
    if (rel_has_prefix(rel, reserved)) {
      throw forbidden("this path is reserved for server metadata",
                      "RESERVED_PATH");
    }
  }
}

void PathGuard::reject_reserved_abs(const fs::path& abs) const {
  for (const fs::path& reserved : reserved_canon_) {
    if (is_within(abs, reserved)) {
      throw forbidden("this path is reserved for server metadata",
                      "RESERVED_PATH");
    }
  }
}

std::string PathGuard::normalize(std::string_view user_path) const {
  std::string decoded;
  if (!percent_decode(user_path, decoded)) {
    throw bad_request("malformed percent encoding in path", "INVALID_ENCODING");
  }
  if (decoded.find('\0') != std::string::npos) {
    throw bad_request("path contains a NUL byte", "INVALID_PATH");
  }
  if (!is_valid_utf8(decoded)) {
    throw bad_request("path is not valid UTF-8", "INVALID_PATH");
  }
  if (decoded.size() > kMaxPathBytes) {
    throw bad_request("path is too long", "INVALID_PATH");
  }

  // Backslashes are treated as separators so the Windows variants of traversal
  // attacks land in the same validation path as the POSIX ones.
  std::string unified;
  unified.reserve(decoded.size());
  for (char c : decoded) unified.push_back(c == '\\' ? '/' : c);

  // Reject UNC / device / loopback network forms outright instead of folding
  // them into a relative path.
  if (unified.size() >= 2 && unified[0] == '/' && unified[1] == '/') {
    throw path_traversal("network or device paths are not allowed");
  }

  std::vector<std::string> segments;
  size_t i = 0;
  while (i < unified.size()) {
    size_t j = unified.find('/', i);
    if (j == std::string::npos) j = unified.size();
    std::string seg = unified.substr(i, j - i);
    i = j + 1;
    if (seg.empty() || seg == ".") continue;
    if (seg == "..") {
      throw path_traversal("'..' segments are not allowed");
    }
    if (seg.size() > kMaxSegmentBytes) {
      throw bad_request("path segment is too long", "INVALID_PATH");
    }
    if (seg.front() == ' ') {
      throw bad_request("path segment must not start with a space",
                        "INVALID_PATH");
    }
    if (seg.back() == ' ' || seg.back() == '.') {
      throw bad_request("path segment must not end with a space or dot",
                        "INVALID_PATH");
    }
    for (char c : seg) {
      if (is_forbidden_char(c)) {
        throw bad_request("path segment contains an illegal character",
                          "INVALID_PATH");
      }
    }
    if (is_windows_reserved_name(seg)) {
      throw bad_request("path segment uses a reserved device name",
                        "INVALID_PATH");
    }
    segments.push_back(std::move(seg));
  }

  std::string rel;
  for (size_t k = 0; k < segments.size(); ++k) {
    if (k) rel.push_back('/');
    rel += segments[k];
  }
  reject_reserved_rel(rel);
  return rel;
}

fs::path PathGuard::resolve(std::string_view user_path) const {
  return resolve_normalized(normalize(user_path));
}

fs::path PathGuard::resolve_normalized(std::string_view rel_path) const {
  reject_reserved_rel(rel_path);
  fs::path abs = rel_path.empty() ? root_ : (root_ / fs::u8path(rel_path));

  std::error_code ec;
  fs::path canon = fs::weakly_canonical(abs, ec);
  if (ec) {
    // Permission problems or dangling links: fall back to a lexical check. The
    // path is still safe because '..' and absolute components never survive
    // normalisation.
    canon = abs.lexically_normal();
    if (!is_within(canon, root_) && !is_within(canon, root_canon_)) {
      throw path_traversal();
    }
    reject_reserved_abs(canon);
    return abs.lexically_normal();
  }
  if (!is_within(canon, root_canon_) && !is_within(canon, root_)) {
    throw path_traversal("path resolves outside the server root");
  }
  reject_reserved_abs(canon);
  return abs.lexically_normal();
}

std::string PathGuard::to_rel(const fs::path& abs) const {
  fs::path rel = abs.lexically_relative(root_);
  const std::string s = rel.generic_u8string();
  if (s == "." || s.empty()) return {};
  if (s.rfind("..", 0) == 0) {
    throw internal_error("path is outside of the server root", "INTERNAL");
  }
  reject_reserved_rel(s);
  return s;
}

}  // namespace fsrv
