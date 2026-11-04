/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/kernel/xboxkrnl/xecrypt_md5.h"

#include "third_party/catch/include/catch.hpp"

namespace {

using xe::kernel::xboxkrnl::XECRYPT_MD5_STATE;
using xe::kernel::xboxkrnl::XeCryptMd5;
using xe::kernel::xboxkrnl::XeCryptMd5Final;
using xe::kernel::xboxkrnl::XeCryptMd5Init;
using xe::kernel::xboxkrnl::XeCryptMd5Update;

using Digest = std::array<uint8_t, 16>;

Digest FromHex(std::string_view hex) {
  Digest digest{};
  for (size_t i = 0; i < digest.size(); i++) {
    digest[i] = static_cast<uint8_t>(
        std::stoi(std::string(hex.substr(i * 2, 2)), nullptr, 16));
  }
  return digest;
}

const uint8_t* Bytes(std::string_view text) {
  return reinterpret_cast<const uint8_t*>(text.data());
}

Digest Md5(std::string_view message) {
  Digest digest;
  XeCryptMd5(Bytes(message), static_cast<uint32_t>(message.size()), nullptr, 0,
             nullptr, 0, digest.data(), static_cast<uint32_t>(digest.size()));
  return digest;
}

Digest Final(XECRYPT_MD5_STATE& state) {
  Digest digest;
  XeCryptMd5Final(&state, digest.data(), static_cast<uint32_t>(digest.size()));
  return digest;
}

}  // namespace

TEST_CASE("XeCryptMd5_known_answers", "[crypt]") {
  // RFC 1321 test suite.
  REQUIRE(Md5("") == FromHex("d41d8cd98f00b204e9800998ecf8427e"));
  REQUIRE(Md5("a") == FromHex("0cc175b9c0f1b6a831c399e269772661"));
  REQUIRE(Md5("abc") == FromHex("900150983cd24fb0d6963f7d28e17f72"));
  REQUIRE(Md5("message digest") == FromHex("f96b697d7cb7938d525a2f31aaf161d0"));
  REQUIRE(Md5("abcdefghijklmnopqrstuvwxyz") ==
          FromHex("c3fcd3d76192e4007dfb496cca67e13b"));
}

TEST_CASE("XeCryptMd5Update_streams_across_blocks", "[crypt]") {
  const std::vector<uint8_t> message(1000000, 'a');
  XECRYPT_MD5_STATE state;
  XeCryptMd5Init(&state);
  size_t offset = 0;
  size_t chunk = 1;
  while (offset < message.size()) {
    const size_t size = std::min(chunk, message.size() - offset);
    XeCryptMd5Update(&state, message.data() + offset,
                     static_cast<uint32_t>(size));
    offset += size;
    chunk = chunk * 3 % 1000 + 1;
  }
  REQUIRE(Final(state) == FromHex("7707d6ae4e027c70eea2a935c2296f21"));
}

TEST_CASE("XeCryptMd5_state_layout", "[crypt]") {
  // A 32-bit byte count, then A to D as big-endian words, then the buffer, in
  // 0x54 bytes.
  alignas(8) std::array<uint8_t, 0x54 + 8> raw;
  raw.fill(0xCC);
  auto* state = reinterpret_cast<XECRYPT_MD5_STATE*>(raw.data());
  XeCryptMd5Init(state);
  const uint8_t init[] = {0x00, 0x00, 0x00, 0x00, 0x67, 0x45, 0x23,
                          0x01, 0xEF, 0xCD, 0xAB, 0x89, 0x98, 0xBA,
                          0xDC, 0xFE, 0x10, 0x32, 0x54, 0x76};
  REQUIRE(std::memcmp(raw.data(), init, sizeof(init)) == 0);

  const std::vector<uint8_t> message(63, 'a');
  XeCryptMd5Update(state, message.data(), 63);
  REQUIRE(raw[3] == 63);
  REQUIRE(std::all_of(raw.begin() + 0x54, raw.end(),
                      [](uint8_t b) { return b == 0xCC; }));
}

TEST_CASE("XeCryptMd5Final_keeps_count_and_last_block", "[crypt]") {
  XECRYPT_MD5_STATE state;
  XeCryptMd5Init(&state);
  XeCryptMd5Update(&state, Bytes("abc"), 3);
  const Digest digest = Final(state);

  REQUIRE(state.count == 3);
  REQUIRE(std::memcmp(state.state, digest.data(), digest.size()) == 0);
  std::array<uint8_t, 64> block{'a', 'b', 'c', 0x80};
  block[56] = 0x18;
  REQUIRE(std::memcmp(state.buffer, block.data(), block.size()) == 0);
}

TEST_CASE("XeCryptMd5Final_32bit_bit_count", "[crypt]") {
  // Only the low 32 bits of the bit count are padded in, so 0x20000003 bytes
  // pad like 3.
  XECRYPT_MD5_STATE state;
  XeCryptMd5Init(&state);
  XeCryptMd5Update(&state, Bytes("abc"), 3);
  state.count = 0x20000003;
  REQUIRE(Final(state) == FromHex("900150983cd24fb0d6963f7d28e17f72"));
}
