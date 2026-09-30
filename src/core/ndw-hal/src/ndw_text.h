// Hex and EUIs as NDW firmware writes and reads them, for the host protocol.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <string>

namespace ndw {
namespace text {

// Sixteen lowercase hex characters, most significant first: an EUI as printed.
inline std::string hex64(uint64_t v) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)v);
  return buf;
}

inline std::string hexBytes(const uint8_t* b, size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    out += digits[b[i] >> 4];
    out += digits[b[i] & 15];
  }
  return out;
}

inline int hexDigit(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// Hex of exactly `bytes` bytes, colons, spaces and dashes allowed, into out.
inline bool parseHex(const char* s, uint8_t* out, size_t bytes) {
  size_t n = 0;
  int hi = -1;
  for (; *s; s++) {
    if (*s == ':' || *s == ' ' || *s == '-') continue;
    int v = hexDigit(*s);
    if (v < 0) return false;
    if (hi < 0) {
      hi = v;
    } else {
      if (n >= bytes) return false;
      out[n++] = (uint8_t)(hi << 4 | v);
      hi = -1;
    }
  }
  return hi < 0 && n == bytes;
}

// Hex of any even length up to `max` bytes, nothing between, into out.
inline bool unhex(const std::string& s, uint8_t* out, size_t max, size_t& len) {
  if (s.size() % 2 || s.size() / 2 > max) return false;
  len = s.size() / 2;
  for (size_t i = 0; i < len; i++) {
    int h = hexDigit(s[2 * i]), l = hexDigit(s[2 * i + 1]);
    if (h < 0 || l < 0) return false;
    out[i] = (uint8_t)(h << 4 | l);
  }
  return true;
}

inline bool parseEui(const char* s, uint64_t* out) {
  uint8_t b[8];
  if (!parseHex(s, b, 8)) return false;
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = v << 8 | b[i];
  *out = v;
  return true;
}

// An EUI the way Basics Station writes one: eight pairs, dashes between.
inline std::string dashed(uint64_t v) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%02x-%02x-%02x-%02x-%02x-%02x-%02x-%02x", (unsigned)(v >> 56) & 0xff,
           (unsigned)(v >> 48) & 0xff, (unsigned)(v >> 40) & 0xff, (unsigned)(v >> 32) & 0xff,
           (unsigned)(v >> 24) & 0xff, (unsigned)(v >> 16) & 0xff, (unsigned)(v >> 8) & 0xff, (unsigned)v & 0xff);
  return buf;
}

inline std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

inline bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

}  // namespace text
}  // namespace ndw
