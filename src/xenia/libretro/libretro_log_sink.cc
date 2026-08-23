/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/libretro/libretro_log_sink.h"

namespace xe {
namespace libretro {

namespace {

// Log lines are formatted as "<prefix>> f:....." by the logger, so the level
// can be recovered from the first character.
retro_log_level LevelForLine(const std::string& line) {
  if (line.size() < 2 || line[1] != '>') {
    return RETRO_LOG_INFO;
  }
  switch (line[0]) {
    case logging::kPrefixCharError:
      return RETRO_LOG_ERROR;
    case logging::kPrefixCharWarning:
      return RETRO_LOG_WARN;
    case logging::kPrefixCharDebug:
      return RETRO_LOG_DEBUG;
    default:
      return RETRO_LOG_INFO;
  }
}

// The logger writes "<c>> f:<frame> <thread> <message>". Both the frame and
// the thread change between otherwise identical complaints, so comparisons
// for repeat suppression have to start at the message itself.
std::string MessageBody(const std::string& line) {
  size_t position = 0;
  for (int token = 0; token < 3; ++token) {
    position = line.find(' ', position);
    if (position == std::string::npos) {
      return line;
    }
    ++position;
  }
  return line.substr(position);
}

// A run of identical messages is reported this often rather than only when
// it ends, so an unbroken flood still shows up in the log.
constexpr uint64_t kSuppressionReportInterval = 1000;

}  // namespace

void LibretroLogSink::Write(const char* buf, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    const char c = buf[i];
    if (c == '\n') {
      EmitLine();
    } else if (c != '\r') {
      line_.push_back(c);
    }
  }
}

void LibretroLogSink::Flush() {
  if (!line_.empty()) {
    EmitLine();
  }
  ReportSuppressed();
}

void LibretroLogSink::ReportSuppressed() {
  if (!suppressed_ || !log_cb_) {
    suppressed_ = 0;
    return;
  }
  log_cb_(RETRO_LOG_WARN,
          "[xenia] previous message repeated %llu more times\n",
          static_cast<unsigned long long>(suppressed_));
  suppressed_ = 0;
}

void LibretroLogSink::Forward(const std::string& line) {
  if (log_cb_) {
    log_cb_(LevelForLine(line), "%s\n", line.c_str());
  }
}

void LibretroLogSink::EmitLine() {
  if (line_.empty()) {
    return;
  }

  std::string body = MessageBody(line_);
  if (body == last_body_) {
    // The same complaint again. Swallow it, but say so periodically so a
    // flood that never ends is still visible.
    if (++suppressed_ % kSuppressionReportInterval == 0) {
      ReportSuppressed();
    }
    line_.clear();
    return;
  }

  ReportSuppressed();
  Forward(line_);
  last_body_ = std::move(body);
  line_.clear();
}

}  // namespace libretro
}  // namespace xe
