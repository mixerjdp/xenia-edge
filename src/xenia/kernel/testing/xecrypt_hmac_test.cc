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
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/kernel/xboxkrnl/xecrypt_sha.h"

#include "third_party/catch/include/catch.hpp"

namespace {

using xe::kernel::xboxkrnl::XECRYPT_HMACSHA_STATE;
using xe::kernel::xboxkrnl::XeCryptHmacSha;
using xe::kernel::xboxkrnl::XeCryptHmacShaFinal;
using xe::kernel::xboxkrnl::XeCryptHmacShaInit;
using xe::kernel::xboxkrnl::XeCryptHmacShaUpdate;

using Bytes = std::vector<uint8_t>;
using Digest = std::array<uint8_t, 20>;

Bytes FromText(std::string_view text) {
  return Bytes(text.begin(), text.end());
}

Digest FromHex(std::string_view hex) {
  Digest digest{};
  for (size_t i = 0; i < digest.size(); i++) {
    digest[i] = static_cast<uint8_t>(
        std::stoi(std::string(hex.substr(i * 2, 2)), nullptr, 16));
  }
  return digest;
}

struct Vector {
  Bytes key;
  Bytes message;
  Digest digest;
};

// RFC 2202 HMAC-SHA1 test cases 1-5, keys up to a block.
std::vector<Vector> Rfc2202() {
  Bytes key_4(25);
  std::iota(key_4.begin(), key_4.end(), uint8_t(1));
  return {
      {Bytes(20, 0x0B), FromText("Hi There"),
       FromHex("b617318655057264e28bc0b6fb378c8ef146be00")},
      {FromText("Jefe"), FromText("what do ya want for nothing?"),
       FromHex("effcdf6ae5eb2fa2d27416d5f184df9c259a7c79")},
      {Bytes(20, 0xAA), Bytes(50, 0xDD),
       FromHex("125d7342b9ac11cd91a39af48aa17b4f63f175d3")},
      {key_4, Bytes(50, 0xCD),
       FromHex("4c9007f4026250c6bc8414f9bf50c86c2d7235da")},
      {Bytes(20, 0x0C), FromText("Test With Truncation"),
       FromHex("4c1a03424b55e07fe7f27be1d58bb9324a9a5a04")},
  };
}

// RFC 2202 test cases 6 and 7 use 80-byte keys, which the console truncates to
// a block instead of hashing them.
std::vector<Vector> LongKeys() {
  return {
      {Bytes(80, 0xAA),
       FromText("Test Using Larger Than Block-Size Key - Hash Key First"),
       FromHex("070a98992c4c1a83474cb780fc564608df3cf503")},
      {Bytes(80, 0xAA),
       FromText("Test Using Larger Than Block-Size Key and Larger Than One "
                "Block-Size Data"),
       FromHex("a6aed3e73996c8be5fc7ba1260fd81172663ec65")},
  };
}

Digest OneShot(const Vector& v) {
  Digest digest;
  XeCryptHmacSha(v.key.data(), static_cast<uint32_t>(v.key.size()),
                 v.message.data(), static_cast<uint32_t>(v.message.size()),
                 nullptr, 0, nullptr, 0, digest.data(),
                 static_cast<uint32_t>(digest.size()));
  return digest;
}

Digest Streamed(const Vector& v) {
  XECRYPT_HMACSHA_STATE state;
  XeCryptHmacShaInit(&state, v.key.data(), static_cast<uint32_t>(v.key.size()));
  const uint32_t split = static_cast<uint32_t>(v.message.size() / 3);
  XeCryptHmacShaUpdate(&state, v.message.data(), split);
  XeCryptHmacShaUpdate(&state, v.message.data() + split,
                       static_cast<uint32_t>(v.message.size()) - split);
  Digest digest;
  XeCryptHmacShaFinal(&state, digest.data(),
                      static_cast<uint32_t>(digest.size()));
  return digest;
}

}  // namespace

TEST_CASE("XeCryptHmacSha_rfc2202", "[crypt]") {
  for (const auto& v : Rfc2202()) {
    REQUIRE(OneShot(v) == v.digest);
  }
}

TEST_CASE("XeCryptHmacSha_truncates_long_keys", "[crypt]") {
  for (const auto& v : LongKeys()) {
    REQUIRE(OneShot(v) == v.digest);
  }
}

TEST_CASE("XeCryptHmacShaFinal_matches_known_answers", "[crypt]") {
  for (const auto& v : Rfc2202()) {
    REQUIRE(Streamed(v) == v.digest);
  }
  for (const auto& v : LongKeys()) {
    REQUIRE(Streamed(v) == v.digest);
  }
}

TEST_CASE("XeCryptHmacSha_joins_inputs_and_truncates_output", "[crypt]") {
  const Bytes key(20, 0x0C);
  const std::string_view inputs[] = {"Test ", "With Trunc", "ation"};
  std::array<uint8_t, 24> out;
  out.fill(0xCC);
  XeCryptHmacSha(key.data(), static_cast<uint32_t>(key.size()),
                 reinterpret_cast<const uint8_t*>(inputs[0].data()),
                 static_cast<uint32_t>(inputs[0].size()),
                 reinterpret_cast<const uint8_t*>(inputs[1].data()),
                 static_cast<uint32_t>(inputs[1].size()),
                 reinterpret_cast<const uint8_t*>(inputs[2].data()),
                 static_cast<uint32_t>(inputs[2].size()), out.data(), 12);
  const Digest expected = FromHex("4c1a03424b55e07fe7f27be1d58bb9324a9a5a04");
  REQUIRE(std::equal(out.begin(), out.begin() + 12, expected.begin()));
  REQUIRE(std::all_of(out.begin() + 12, out.end(),
                      [](uint8_t b) { return b == 0xCC; }));
}
