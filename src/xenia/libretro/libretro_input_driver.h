/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_LIBRETRO_LIBRETRO_INPUT_DRIVER_H_
#define XENIA_LIBRETRO_LIBRETRO_INPUT_DRIVER_H_

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "xenia/hid/input_driver.h"
#include "xenia/libretro/libretro.h"

namespace xe {
namespace libretro {

// Feeds the guest from the frontend's RetroPad. The mapping is direct - a
// RetroPad is modelled on this exact controller - so the only real work is
// scaling the analog ranges and byte order, which X_INPUT_GAMEPAD handles.
//
// The frontend's input is polled on its own thread inside retro_run, while the
// guest asks for state from its own threads, so the polled snapshot is kept
// behind a lock rather than calling into the frontend from guest threads.
class LibretroInputDriver final : public hid::InputDriver {
 public:
  static constexpr uint32_t kMaxPorts = 4;

  explicit LibretroInputDriver(ui::Window* window, size_t window_z_order);
  ~LibretroInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           hid::X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, hid::X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index,
                    hid::X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        hid::X_INPUT_KEYSTROKE* out_keystroke) override;

  hid::InputType GetInputType() const override {
    return hid::InputType::Controller;
  }

  std::vector<hid::InputDeviceInfo> EnumerateDevices() override;

  // Snapshots every port from the frontend. Called on the frontend's thread
  // inside retro_run, right after input_poll.
  void Poll(retro_input_state_t input_state_cb, bool bitmask_supported);

 private:
  struct PortState {
    uint16_t buttons = 0;
    uint8_t left_trigger = 0;
    uint8_t right_trigger = 0;
    int16_t thumb_lx = 0;
    int16_t thumb_ly = 0;
    int16_t thumb_rx = 0;
    int16_t thumb_ry = 0;
    // Bumped whenever the state differs, which is what the guest uses to
    // notice a change without diffing the struct itself.
    uint32_t packet_number = 1;
  };

  std::mutex mutex_;
  std::array<PortState, kMaxPorts> ports_;
};

// Called from retro_run with the frontend's input callbacks, after
// input_poll. Snapshots every port for the guest to read.
void PollLibretroInput(retro_input_state_t input_state_cb,
                       bool bitmask_supported);

// Set once at startup so the driver and the poll share one instance.
void SetActiveInputDriver(LibretroInputDriver* driver);

}  // namespace libretro
}  // namespace xe

#endif  // XENIA_LIBRETRO_LIBRETRO_INPUT_DRIVER_H_
