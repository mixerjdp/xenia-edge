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

  retro_log_printf_t log_cb_;
  std::string line_;
};

}  // namespace libretro
}  // namespace xe

#endif  // XENIA_LIBRETRO_LIBRETRO_LOG_SINK_H_
