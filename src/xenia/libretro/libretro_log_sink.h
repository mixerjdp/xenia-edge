/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_LIBRETRO_LIBRETRO_LOG_SINK_H_
#define XENIA_LIBRETRO_LIBRETRO_LOG_SINK_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "xenia/base/logging.h"
#include "xenia/libretro/libretro.h"

namespace xe {
namespace libretro {

// Mirrors the xenia log into the frontend's logger. The sink is handed raw
// buffers that may split or merge lines, so it reassembles whole lines before
// forwarding them.
class LibretroLogSink final : public LogSink {
 public:
  explicit LibretroLogSink(retro_log_printf_t log_cb) : log_cb_(log_cb) {}

  void Write(const char* buf, size_t size) override;
  void Flush() override;

 private:
  void EmitLine();
  // Forwards a line, first reporting anything that was suppressed before it.
  void Forward(const std::string& line);
  void ReportSuppressed();

  retro_log_printf_t log_cb_;
  std::string line_;

  // Repeat suppression. A guest can make Xenia log the same complaint on every
  // draw - Forza Motorsport does thousands a frame - and forwarding each one
  // costs the frontend a formatted write, which drags the whole emulator down.
  // Only the message body is compared: the frame number and thread id in the
  // prefix differ between otherwise identical lines.
  std::string last_body_;
  uint64_t suppressed_ = 0;
};

}  // namespace libretro
}  // namespace xe

#endif  // XENIA_LIBRETRO_LIBRETRO_LOG_SINK_H_
