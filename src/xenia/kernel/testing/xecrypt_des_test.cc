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
#include <bit>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/kernel/xboxkrnl/xecrypt_des.h"

#include "third_party/catch/include/catch.hpp"

namespace {

using xe::kernel::xboxkrnl::XECRYPT_DES3_STATE;
using xe::kernel::xboxkrnl::XECRYPT_DES_STATE;
using xe::kernel::xboxkrnl::XeCryptDes3Cbc;
using xe::kernel::xboxkrnl::XeCryptDes3Ecb;
using xe::kernel::xboxkrnl::XeCryptDes3Key;
using xe::kernel::xboxkrnl::XeCryptDesCbc;
using xe::kernel::xboxkrnl::XeCryptDesEcb;
using xe::kernel::xboxkrnl::XeCryptDesKey;
using xe::kernel::xboxkrnl::XeCryptDesParity;

using Bytes = std::vector<uint8_t>;

Bytes FromHex(std::string_view hex) {
  Bytes bytes(hex.size() / 2);
  for (size_t i = 0; i < bytes.size(); i++) {
    bytes[i] = static_cast<uint8_t>(
        std::stoi(std::string(hex.substr(i * 2, 2)), nullptr, 16));
  }
  return bytes;
}

// NIST SP 800-67 three-key TDEA example.
const Bytes kDes3Key =
    FromHex("0123456789ABCDEF23456789ABCDEF01456789ABCDEF0123");
const std::string_view kDes3Plaintext = "The qufck brown fox jump";

}  // namespace

TEST_CASE("XeCryptDesEcb_known_answer", "[crypt]") {
  XECRYPT_DES_STATE state;
  XeCryptDesKey(&state, FromHex("133457799BBCDFF1").data());
  Bytes block = FromHex("0123456789ABCDEF");
  XeCryptDesEcb(&state, block.data(), block.data(), true);
  REQUIRE(block == FromHex("85E813540F0AB405"));
  XeCryptDesEcb(&state, block.data(), block.data(), false);
  REQUIRE(block == FromHex("0123456789ABCDEF"));
}

TEST_CASE("XeCryptDesCbc_updates_feed_and_skips_partial_block", "[crypt]") {
  // FIPS 81 CBC example.
  XECRYPT_DES_STATE state;
  XeCryptDesKey(&state, FromHex("0123456789ABCDEF").data());
  const Bytes iv = FromHex("1234567890ABCDEF");
  const std::string_view plaintext = "Now is the time for all ";
  const Bytes ciphertext =
      FromHex("E5C7CDDE872BF27C43E934008C389C0F683788499A7C05F6");

  Bytes data(plaintext.begin(), plaintext.end());
  data.resize(28, 0xCC);
  Bytes feed = iv;
  XeCryptDesCbc(&state, data.data(), 28, data.data(), feed.data(), true);
  REQUIRE(std::equal(ciphertext.begin(), ciphertext.end(), data.begin()));
  REQUIRE(std::all_of(data.begin() + 24, data.end(),
                      [](uint8_t b) { return b == 0xCC; }));
  REQUIRE(feed == FromHex("683788499A7C05F6"));

  feed = iv;
  XeCryptDesCbc(&state, data.data(), 24, data.data(), feed.data(), false);
  REQUIRE(std::equal(plaintext.begin(), plaintext.end(), data.begin()));
  REQUIRE(feed == FromHex("683788499A7C05F6"));
}

TEST_CASE("XeCryptDes3Ecb_known_answer", "[crypt]") {
  XECRYPT_DES3_STATE state;
  XeCryptDes3Key(&state, kDes3Key.data());
  Bytes data(kDes3Plaintext.begin(), kDes3Plaintext.end());
  for (size_t i = 0; i < data.size(); i += 8) {
    XeCryptDes3Ecb(&state, data.data() + i, data.data() + i, true);
  }
  REQUIRE(data == FromHex("A826FD8CE53B855FCCE21C8112256FE6"
                          "68D5C05DD9B6B900"));
  for (size_t i = 0; i < data.size(); i += 8) {
    XeCryptDes3Ecb(&state, data.data() + i, data.data() + i, false);
  }
  REQUIRE(data == Bytes(kDes3Plaintext.begin(), kDes3Plaintext.end()));
}

TEST_CASE("XeCryptDes3Cbc_updates_feed_and_skips_partial_block", "[crypt]") {
  XECRYPT_DES3_STATE state;
  XeCryptDes3Key(&state, kDes3Key.data());
  const Bytes iv = FromHex("F69F2445DF4F9B17");
  const Bytes ciphertext =
      FromHex("A5C282BAD0DE3774BECD2E04386B589FB5057D8552FC4336");

  Bytes data(kDes3Plaintext.begin(), kDes3Plaintext.end());
  data.resize(28, 0xCC);
  Bytes feed = iv;
  XeCryptDes3Cbc(&state, data.data(), 28, data.data(), feed.data(), true);
  REQUIRE(std::equal(ciphertext.begin(), ciphertext.end(), data.begin()));
  REQUIRE(std::all_of(data.begin() + 24, data.end(),
                      [](uint8_t b) { return b == 0xCC; }));
  REQUIRE(feed == FromHex("B5057D8552FC4336"));

  feed = iv;
  XeCryptDes3Cbc(&state, data.data(), 24, data.data(), feed.data(), false);
  REQUIRE(
      std::equal(kDes3Plaintext.begin(), kDes3Plaintext.end(), data.begin()));
  REQUIRE(feed == FromHex("B5057D8552FC4336"));
}

TEST_CASE("XeCryptDesParity_sets_odd_parity", "[crypt]") {
  // Every byte value, so the length isn't a whole number of DES keys either.
  Bytes input(255);
  for (size_t i = 0; i < input.size(); i++) {
    input[i] = static_cast<uint8_t>(i + 1);
  }
  Bytes output(input.size());
  XeCryptDesParity(input.data(), static_cast<uint32_t>(input.size()),
                   output.data());
  for (size_t i = 0; i < input.size(); i++) {
    INFO("byte " << int(input[i]));
    REQUIRE((output[i] & 0xFE) == (input[i] & 0xFE));
    REQUIRE(std::popcount(output[i]) % 2 == 1);
  }

  Bytes in_place = {0x00, 0x01, 0x03};
  const Bytes expected = {0x01, 0x01, 0x02};
  XeCryptDesParity(in_place.data(), 3, in_place.data());
  REQUIRE(in_place == expected);
}
