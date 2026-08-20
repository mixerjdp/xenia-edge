/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/libretro/libretro_input_driver.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <string>

#include "xenia/base/logging.h"

namespace xe {
namespace libretro {

namespace {

LibretroInputDriver* g_active_driver = nullptr;

struct ButtonMapping {
  unsigned retro_id;
  uint16_t xinput_bit;
};

// A RetroPad is a 360 pad with different labels, so this is one-to-one. The
// face buttons keep their RetroPad positions rather than their names: RetroPad
// B is the bottom button, which is A on a 360 pad.
constexpr ButtonMapping kButtonMap[] = {
    {RETRO_DEVICE_ID_JOYPAD_UP, hid::X_INPUT_GAMEPAD_DPAD_UP},
    {RETRO_DEVICE_ID_JOYPAD_DOWN, hid::X_INPUT_GAMEPAD_DPAD_DOWN},
    {RETRO_DEVICE_ID_JOYPAD_LEFT, hid::X_INPUT_GAMEPAD_DPAD_LEFT},
    {RETRO_DEVICE_ID_JOYPAD_RIGHT, hid::X_INPUT_GAMEPAD_DPAD_RIGHT},
    {RETRO_DEVICE_ID_JOYPAD_START, hid::X_INPUT_GAMEPAD_START},
    {RETRO_DEVICE_ID_JOYPAD_SELECT, hid::X_INPUT_GAMEPAD_BACK},
    {RETRO_DEVICE_ID_JOYPAD_L3, hid::X_INPUT_GAMEPAD_LEFT_THUMB},
    {RETRO_DEVICE_ID_JOYPAD_R3, hid::X_INPUT_GAMEPAD_RIGHT_THUMB},
    {RETRO_DEVICE_ID_JOYPAD_L, hid::X_INPUT_GAMEPAD_LEFT_SHOULDER},
    {RETRO_DEVICE_ID_JOYPAD_R, hid::X_INPUT_GAMEPAD_RIGHT_SHOULDER},
    {RETRO_DEVICE_ID_JOYPAD_B, hid::X_INPUT_GAMEPAD_A},
    {RETRO_DEVICE_ID_JOYPAD_A, hid::X_INPUT_GAMEPAD_B},
    {RETRO_DEVICE_ID_JOYPAD_Y, hid::X_INPUT_GAMEPAD_X},
    {RETRO_DEVICE_ID_JOYPAD_X, hid::X_INPUT_GAMEPAD_Y},
};

// Triggers are analog on a 360 pad but the RetroPad exposes them as buttons
// unless the frontend reports analog values, so take whichever is larger.
uint8_t ReadTrigger(retro_input_state_t cb, unsigned port, unsigned button_id,
                    unsigned analog_id, bool pressed) {
  const int16_t analog = cb(port, RETRO_DEVICE_ANALOG,
                            RETRO_DEVICE_INDEX_ANALOG_BUTTON, analog_id);
  const int32_t from_analog = analog > 0 ? (analog * 255) / 0x7FFF : 0;
  const int32_t from_button = pressed ? 255 : 0;
  (void)button_id;
  return static_cast<uint8_t>(std::max(from_analog, from_button));
}

// The guest's Y axis points the opposite way to libretro's, and -32768 has no
// positive counterpart to negate.
int16_t FlipAxis(int16_t value) {
  return value == INT16_MIN ? INT16_MAX : static_cast<int16_t>(-value);
}

}  // namespace

LibretroInputDriver::LibretroInputDriver(ui::Window* window,
                                         size_t window_z_order)
    : hid::InputDriver(window, window_z_order) {}

LibretroInputDriver::~LibretroInputDriver() {
  if (g_active_driver == this) {
    g_active_driver = nullptr;
  }
}

X_STATUS LibretroInputDriver::Setup() { return X_STATUS_SUCCESS; }

std::vector<hid::InputDeviceInfo> LibretroInputDriver::EnumerateDevices() {
  // The frontend always has four ports, whether or not real pads are plugged
  // into them. Reporting all four keeps guest slots stable, and titles that
  // wait for a controller find one immediately.
  std::vector<hid::InputDeviceInfo> devices;
  for (uint32_t port = 0; port < kMaxPorts; ++port) {
    hid::InputDeviceInfo info;
    info.driver_slot = static_cast<uint8_t>(port);
    info.stable_id = "libretro-port-" + std::to_string(port);
    info.display_name = "RetroPad " + std::to_string(port + 1);
    info.subtype = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    info.preferred_slot = static_cast<int8_t>(port);
    info.auto_bind = true;
    devices.push_back(std::move(info));
  }
  return devices;
}

X_RESULT LibretroInputDriver::GetCapabilities(uint32_t user_index,
                                              uint32_t flags,
                                              hid::X_INPUT_CAPABILITIES* out_caps) {
  if (user_index >= kMaxPorts) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::memset(out_caps, 0, sizeof(*out_caps));
  out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
  out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
  out_caps->flags = 0;
  // Every field the pad can report is set to its maximum, which is how XInput
  // advertises "this control exists and is fully analog/digital".
  out_caps->gamepad.buttons = 0xFFFF;
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = static_cast<int16_t>(0xFFC0);
  out_caps->gamepad.thumb_ly = static_cast<int16_t>(0xFFC0);
  out_caps->gamepad.thumb_rx = static_cast<int16_t>(0xFFC0);
  out_caps->gamepad.thumb_ry = static_cast<int16_t>(0xFFC0);
  out_caps->vibration.left_motor_speed = 0xFFFF;
  out_caps->vibration.right_motor_speed = 0xFFFF;
  return X_ERROR_SUCCESS;
}

X_RESULT LibretroInputDriver::GetState(uint32_t user_index,
                                       hid::X_INPUT_STATE* out_state) {
  if (user_index >= kMaxPorts) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const PortState& port = ports_[user_index];
  out_state->packet_number = port.packet_number;
  out_state->gamepad.buttons = port.buttons;
  out_state->gamepad.left_trigger = port.left_trigger;
  out_state->gamepad.right_trigger = port.right_trigger;
  out_state->gamepad.thumb_lx = port.thumb_lx;
  out_state->gamepad.thumb_ly = port.thumb_ly;
  out_state->gamepad.thumb_rx = port.thumb_rx;
  out_state->gamepad.thumb_ry = port.thumb_ry;
  return X_ERROR_SUCCESS;
}

X_RESULT LibretroInputDriver::SetState(uint32_t user_index,
                                       hid::X_INPUT_VIBRATION* vibration) {
  // Rumble goes through RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, which is not
  // wired up yet. Reporting success keeps titles from treating the pad as
  // faulty.
  if (user_index >= kMaxPorts) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT LibretroInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                           hid::X_INPUT_KEYSTROKE* out_keystroke) {
  // Only the virtual keyboard uses this, and there is no keyboard here.
  return X_ERROR_EMPTY;
}

void LibretroInputDriver::Poll(retro_input_state_t input_state_cb,
                               bool bitmask_supported) {
  if (!input_state_cb) {
    return;
  }
  for (uint32_t port = 0; port < kMaxPorts; ++port) {
    PortState next;

    // One call for all buttons where the frontend supports it - the per-button
    // path costs a callback each and this runs every frame on every port.
    int16_t bits = 0;
    if (bitmask_supported) {
      bits = input_state_cb(port, RETRO_DEVICE_JOYPAD, 0,
                            RETRO_DEVICE_ID_JOYPAD_MASK);
    }
    auto pressed = [&](unsigned id) -> bool {
      if (bitmask_supported) {
        return (bits & (1 << id)) != 0;
      }
      return input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, id) != 0;
    };

    for (const ButtonMapping& mapping : kButtonMap) {
      if (pressed(mapping.retro_id)) {
        next.buttons |= mapping.xinput_bit;
      }
    }

    next.left_trigger =
        ReadTrigger(input_state_cb, port, RETRO_DEVICE_ID_JOYPAD_L2,
                    RETRO_DEVICE_ID_JOYPAD_L2, pressed(RETRO_DEVICE_ID_JOYPAD_L2));
    next.right_trigger =
        ReadTrigger(input_state_cb, port, RETRO_DEVICE_ID_JOYPAD_R2,
                    RETRO_DEVICE_ID_JOYPAD_R2, pressed(RETRO_DEVICE_ID_JOYPAD_R2));

    next.thumb_lx = input_state_cb(port, RETRO_DEVICE_ANALOG,
                                   RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                   RETRO_DEVICE_ID_ANALOG_X);
    next.thumb_ly = FlipAxis(input_state_cb(port, RETRO_DEVICE_ANALOG,
                                            RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                            RETRO_DEVICE_ID_ANALOG_Y));
    next.thumb_rx = input_state_cb(port, RETRO_DEVICE_ANALOG,
                                   RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                                   RETRO_DEVICE_ID_ANALOG_X);
    next.thumb_ry = FlipAxis(input_state_cb(port, RETRO_DEVICE_ANALOG,
                                            RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                                            RETRO_DEVICE_ID_ANALOG_Y));

    std::lock_guard<std::mutex> lock(mutex_);
    PortState& current = ports_[port];
    const bool changed = next.buttons != current.buttons ||
                         next.left_trigger != current.left_trigger ||
                         next.right_trigger != current.right_trigger ||
                         next.thumb_lx != current.thumb_lx ||
                         next.thumb_ly != current.thumb_ly ||
                         next.thumb_rx != current.thumb_rx ||
                         next.thumb_ry != current.thumb_ry;
    next.packet_number =
        changed ? current.packet_number + 1 : current.packet_number;
    current = next;
  }
}

void SetActiveInputDriver(LibretroInputDriver* driver) {
  g_active_driver = driver;
}

void PollLibretroInput(retro_input_state_t input_state_cb,
                       bool bitmask_supported) {
  if (g_active_driver) {
    g_active_driver->Poll(input_state_cb, bitmask_supported);
  }
}

}  // namespace libretro
}  // namespace xe
