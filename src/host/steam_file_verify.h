#pragma once

#include "../trt/bridge_crypt32.h"
#include <cstdint>
#include <cstring>
#include <istream>

// Compare installed bytes with sha_content from the authorized depot manifest.
inline bool steam_file_matches(std::istream& input, uint64_t size,
                               const uint8_t expected[20]) {
  trt_hash_ctx hash;
  trt_hash_init(&hash, TRT_HASH_SHA1);
  uint8_t buffer[32768];
  while (size) {
    const auto count = size < sizeof(buffer) ? size : sizeof(buffer);
    if (!input.read(reinterpret_cast<char*>(buffer), count)) return false;
    trt_hash_update(&hash, buffer, static_cast<u32>(count));
    size -= count;
  }
  if (input.peek() != std::istream::traits_type::eof() || input.bad()) return false;
  uint8_t digest[20];
  trt_hash_final(&hash, digest);
  return std::memcmp(digest, expected, sizeof(digest)) == 0;
}
