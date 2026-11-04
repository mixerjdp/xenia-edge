/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/kernel/xboxkrnl/xecrypt_aes.h"

#include "third_party/catch/include/catch.hpp"

namespace {

using xe::kernel::xboxkrnl::XECRYPT_AES_STATE;
using xe::kernel::xboxkrnl::XeCryptAesCbc;
using xe::kernel::xboxkrnl::XeCryptAesEcb;
using xe::kernel::xboxkrnl::XeCryptAesKey;

using Bytes = std::vector<uint8_t>;

Bytes FromHex(std::string_view hex) {
  Bytes bytes(hex.size() / 2);
  for (size_t i = 0; i < bytes.size(); i++) {
    bytes[i] = static_cast<uint8_t>(
        std::stoi(std::string(hex.substr(i * 2, 2)), nullptr, 16));
  }
  return bytes;
}

// NIST SP 800-38A F.2.1 CBC-AES128.
const Bytes kKey = FromHex("2B7E151628AED2A6ABF7158809CF4F3C");
const Bytes kIv = FromHex("000102030405060708090A0B0C0D0E0F");
const Bytes kPlaintext = FromHex(
    "6BC1BEE22E409F96E93D7E117393172AAE2D8A571E03AC9C9EB76FAC45AF8E51"
    "30C81C46A35CE411E5FBC1191A0A52EFF69F2445DF4F9B17AD2B417BE66C3710");
const Bytes kCiphertext = FromHex(
    "7649ABAC8119B246CEE98E9B12E9197D5086CB9B507219EE95DB113A917678B2"
    "73BED6B8E3C1743B7116E69E222295163FF1CAA1681FAC09120ECA307586E1A7");

}  // namespace

TEST_CASE("XeCryptAesKey_schedule_layout", "[crypt]") {
  // keytabenc is the FIPS-197 A.1 expansion. keytabdec holds the decryption
  // round keys in the same round order, InvMixColumns applied to rounds 1-9.
  XECRYPT_AES_STATE state;
  XeCryptAesKey(&state, kKey.data());
  const Bytes enc = FromHex(
      "2B7E151628AED2A6ABF7158809CF4F3CA0FAFE1788542CB123A339392A6C7605"
      "F2C295F27A96B9435935807A7359F67F3D80477D4716FE3E1E237E446D7A883B"
      "EF44A541A8525B7FB671253BDB0BAD00D4D1C6F87C839D87CAF2B8BC11F915BC"
      "6D88A37A110B3EFDDBF98641CA0093FD4E54F70E5F5FC9F384A64FB24EA6DC4F"
      "EAD27321B58DBAD2312BF5607F8D292FAC7766F319FADC2128D12941575C006E"
      "D014F9A8C9EE2589E13F0CC8B6630CA6");
  const Bytes dec = FromHex(
      "2B7E151628AED2A6ABF7158809CF4F3C2B3708A7F262D405BC3EBDBF4B617D62"
      "CC7505EB3E17D1EE82296C51C94811337C1F13F74208C219C021AE480969BF7B"
      "90884413D280860A12A128421BC897396EA30AFCBC238CF6AE82A4B4B54A338D"
      "6EFCD876D2DF54807C5DF034C917C3B912C07647C01F22C7BC42D2F37555114A"
      "DF7D925A1F62B09DA320626ED67573240C7B5A631319EAFEB0398890664CFBB4"
      "D014F9A8C9EE2589E13F0CC8B6630CA6");
  REQUIRE(std::memcmp(state.keytabenc, enc.data(), enc.size()) == 0);
  REQUIRE(std::memcmp(state.keytabdec, dec.data(), dec.size()) == 0);
}

TEST_CASE("XeCryptAesEcb_known_answer", "[crypt]") {
  // FIPS-197 C.1.
  XECRYPT_AES_STATE state;
  XeCryptAesKey(&state, FromHex("000102030405060708090A0B0C0D0E0F").data());
  Bytes block = FromHex("00112233445566778899AABBCCDDEEFF");
  XeCryptAesEcb(&state, block.data(), block.data(), true);
  REQUIRE(block == FromHex("69C4E0D86A7B0430D8CDB78070B4C55A"));
  XeCryptAesEcb(&state, block.data(), block.data(), false);
  REQUIRE(block == FromHex("00112233445566778899AABBCCDDEEFF"));
}

TEST_CASE("XeCryptAesCbc_known_answer", "[crypt]") {
  XECRYPT_AES_STATE state;
  XeCryptAesKey(&state, kKey.data());
  Bytes data = kPlaintext;
  Bytes feed = kIv;
  XeCryptAesCbc(&state, data.data(), static_cast<uint32_t>(data.size()),
                data.data(), feed.data(), true);
  REQUIRE(data == kCiphertext);
  REQUIRE(std::equal(feed.begin(), feed.end(), kCiphertext.end() - 16));

  feed = kIv;
  XeCryptAesCbc(&state, data.data(), static_cast<uint32_t>(data.size()),
                data.data(), feed.data(), false);
  REQUIRE(data == kPlaintext);
  REQUIRE(std::equal(feed.begin(), feed.end(), kCiphertext.end() - 16));
}

TEST_CASE("XeCryptAesCbc_skips_partial_block", "[crypt]") {
  XECRYPT_AES_STATE state;
  XeCryptAesKey(&state, kKey.data());
  Bytes input = kPlaintext;
  Bytes output(input.size(), 0xCC);
  Bytes feed = kIv;
  XeCryptAesCbc(&state, input.data(), 20, output.data(), feed.data(), true);
  REQUIRE(std::equal(output.begin(), output.begin() + 16, kCiphertext.begin()));
  REQUIRE(std::all_of(output.begin() + 16, output.end(),
                      [](uint8_t b) { return b == 0xCC; }));
  REQUIRE(std::equal(feed.begin(), feed.end(), kCiphertext.begin()));
}
