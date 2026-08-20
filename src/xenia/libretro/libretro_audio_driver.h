/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_LIBRETRO_LIBRETRO_AUDIO_DRIVER_H_
#define XENIA_LIBRETRO_LIBRETRO_AUDIO_DRIVER_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "xenia/apu/audio_driver.h"
#include "xenia/apu/audio_system.h"
#include "xenia/base/threading.h"

namespace xe {
namespace libretro {

// Audio driver that parks the guest's frames in a ring buffer for the
// frontend's frame loop to collect, instead of opening a host audio device.
//
// The guest's mixer thread calls SubmitFrame; retro_run calls Drain. Those are
// different threads, hence the lock. The emulator's audio system uses the
// semaphore as back-pressure - it only submits when a slot is free - so the
// semaphore is released as frames are drained, not as they arrive, which is
// what keeps the guest paced to the frontend rather than running ahead into an
// ever-growing buffer.
class LibretroAudioDriver final : public apu::AudioDriver {
 public:
  // Output is always 48 kHz stereo signed 16-bit, the format libretro wants.
  static constexpr uint32_t kOutputChannels = 2;

  LibretroAudioDriver(threading::Semaphore* semaphore, uint32_t frequency,
                      uint32_t channels, bool need_format_conversion);
  ~LibretroAudioDriver() override;

  bool Initialize() override { return true; }
  void Shutdown() override;
  void SubmitFrame(float* samples) override;
  void Pause() override { paused_ = true; }
  void Resume() override { paused_ = false; }
  void SetVolume(float volume) override { volume_ = volume; }

  // Number of stereo sample frames waiting to be drained.
  size_t QueuedFrames();

  // Moves up to max_frames stereo frames into out, summing into whatever is
  // already there so several drivers can share one output buffer. Returns how
  // many frames this driver contributed.
  size_t Drain(int16_t* out, size_t max_frames);

 private:
  threading::Semaphore* semaphore_ = nullptr;
  uint32_t frame_channels_;
  uint32_t channel_samples_;
  bool need_format_conversion_;

  bool paused_ = false;
  float volume_ = 1.0f;

  std::mutex mutex_;
  // Interleaved stereo samples, oldest first.
  std::vector<int16_t> queue_;
  // Frames submitted but not yet acknowledged to the audio system.
  size_t unreleased_frames_ = 0;
  // Samples per submitted frame, to know how much a released slot is worth.
  size_t samples_per_frame_ = 0;
};

// Audio system whose drivers feed the frontend. Every live driver is
// registered here so the frame loop can find them without threading a pointer
// through the emulator.
class LibretroAudioSystem final : public apu::AudioSystem {
 public:
  explicit LibretroAudioSystem(cpu::Processor* processor);
  ~LibretroAudioSystem() override;

  static std::unique_ptr<apu::AudioSystem> Create(cpu::Processor* processor);

  std::string name() const override { return "libretro"; }

  X_STATUS CreateDriver(size_t index, threading::Semaphore* semaphore,
                        apu::AudioDriver** out_driver) override;
  apu::AudioDriver* CreateDriver(threading::Semaphore* semaphore,
                                 uint32_t frequency, uint32_t channels,
                                 bool need_format_conversion) override;
  void DestroyDriver(apu::AudioDriver* driver) override;
};

// Mixes every live driver into out and returns the frame count produced.
// Called from the frontend's frame loop.
size_t DrainAudioDrivers(int16_t* out, size_t max_frames);

}  // namespace libretro
}  // namespace xe

#endif  // XENIA_LIBRETRO_LIBRETRO_AUDIO_DRIVER_H_
