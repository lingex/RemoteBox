#pragma once
#include <stddef.h>
#include <stdint.h>

namespace easyconfig {
inline int digit(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  return c == '+' ? 62 : c == '/' ? 63 : -1;
}
inline int encode(unsigned char *out, size_t capacity, size_t *written,
                  const unsigned char *data, size_t length) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  *written = 0;
  if (capacity <= ((length + 2) / 3) * 4) return -1;
  for (size_t i = 0; i < length; i += 3) {
    uint32_t value = uint32_t(data[i]) << 16;
    if (i + 1 < length) value |= uint32_t(data[i + 1]) << 8;
    if (i + 2 < length) value |= data[i + 2];
    out[(*written)++] = alphabet[(value >> 18) & 63];
    out[(*written)++] = alphabet[(value >> 12) & 63];
    out[(*written)++] = i + 1 < length ? alphabet[(value >> 6) & 63] : '=';
    out[(*written)++] = i + 2 < length ? alphabet[value & 63] : '=';
  }
  out[*written] = 0;
  return 0;
}
inline int decode(unsigned char *out, size_t capacity, size_t *written,
                  const unsigned char *data, size_t length) {
  *written = 0;
  if (length % 4) return -1;
  for (size_t i = 0; i < length; i += 4) {
    const int a = digit(data[i]), b = digit(data[i + 1]);
    const int c = data[i + 2] == '=' ? 0 : digit(data[i + 2]);
    const int d = data[i + 3] == '=' ? 0 : digit(data[i + 3]);
    const size_t count = data[i + 2] == '=' ? 1 : data[i + 3] == '=' ? 2 : 3;
    if (a < 0 || b < 0 || c < 0 || d < 0 || *written + count > capacity ||
        (count < 3 && i + 4 != length) ||
        (count == 1 && (data[i + 3] != '=' || (b & 15))) ||
        (count == 2 && (c & 3))) return -1;
    const uint32_t value = (uint32_t(a) << 18) | (uint32_t(b) << 12) | (uint32_t(c) << 6) | d;
    out[(*written)++] = static_cast<unsigned char>(value >> 16);
    if (count > 1) out[(*written)++] = static_cast<unsigned char>(value >> 8);
    if (count > 2) out[(*written)++] = static_cast<unsigned char>(value);
  }
  return 0;
}
}
