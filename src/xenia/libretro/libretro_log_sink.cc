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
}

void LibretroLogSink::EmitLine() {
  if (log_cb_ && !line_.empty()) {
    log_cb_(LevelForLine(line_), "%s\n", line_.c_str());
  }
  line_.clear();
}

}  // namespace libretro
}  // namespace xe
