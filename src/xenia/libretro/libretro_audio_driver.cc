/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/libretro/libretro_audio_driver.h"

#include <algorithm>
#include <cstring>

#include "xenia/apu/conversion.h"
#include "xenia/base/logging.h"

namespace xe {
namespace libretro {

namespace {

// Live drivers, so the frame loop can drain them without the emulator handing
// out a pointer. Guarded because drivers are created and destroyed on the
// guest's threads while the frontend's thread drains.
std::mutex g_drivers_mutex;
std::vector<LibretroAudioDriver*> g_drivers;

// Roughly a third of a second of stereo audio. Deep enough to ride out a
// stuttering frame loop, shallow enough that the delay stays unnoticeable.
constexpr size_t kMaxQueuedSamples = 48000 / 3 * 2;

int16_t FloatToPcm16(float sample) {
  const float scaled = sample * 32767.0f;
  return static_cast<int16_t>(std::clamp(scaled, -32768.0f, 32767.0f));
}

}  // namespace

LibretroAudioDriver::LibretroAudioDriver(threading::Semaphore* semaphore,
                                         uint32_t frequency, uint32_t channels,
                                         bool need_format_conversion)
    : semaphore_(semaphore),
      frame_channels_(channels),
      need_format_conversion_(need_format_conversion) {
  // Matches the frame sizes the audio system submits: 5.1 comes in 256-sample
  // chunks, the stereo (XMP) path in 768.
  channel_samples_ = frame_channels_ == 2 ? 768 : 256;
  samples_per_frame_ = size_t(channel_samples_) * kOutputChannels;

  if (frequency != kFrameFrequencyDefault) {
    // Nothing resamples here yet; the frontend was told 48 kHz.
    XELOGW("LibretroAudioDriver: unexpected frequency {} (expected {})",
           frequency, kFrameFrequencyDefault);
  }

  std::lock_guard<std::mutex> lock(g_drivers_mutex);
  g_drivers.push_back(this);
}

LibretroAudioDriver::~LibretroAudioDriver() {
  std::lock_guard<std::mutex> lock(g_drivers_mutex);
  g_drivers.erase(std::remove(g_drivers.begin(), g_drivers.end(), this),
                  g_drivers.end());
}

void LibretroAudioDriver::Shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.clear();
  // Anything still owed has to be handed back, or the audio system's worker
  // blocks forever waiting for slots that will never free up.
  if (unreleased_frames_ && semaphore_) {
    semaphore_->Release(static_cast<int>(unreleased_frames_), nullptr);
    unreleased_frames_ = 0;
  }
}

void LibretroAudioDriver::SubmitFrame(float* samples) {
  // Guest audio is 5.1 in big-endian, channel-sequential layout. The shared
  // conversion does the byte swap and the XAudio2-style downmix in one pass;
  // the stereo path is already interleaved little-endian.
  float converted[kFrameSizeMax / sizeof(float)];
  const float* stereo = converted;
  size_t stereo_samples = size_t(channel_samples_) * kOutputChannels;

  if (need_format_conversion_ && frame_channels_ == 6) {
    apu::conversion::sequential_6_BE_to_interleaved_2_LE(converted, samples,
                                                         channel_samples_);
  } else if (frame_channels_ == 2) {
    stereo = samples;
  } else {
    // An unexpected layout would otherwise be written out as noise.
    XELOGW("LibretroAudioDriver: dropping frame with {} channels",
           frame_channels_);
    if (semaphore_) {
      semaphore_->Release(1, nullptr);
    }
    return;
  }

  const float gain = paused_ ? 0.0f : volume_;

  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.size() + stereo_samples > kMaxQueuedSamples) {
    // The frontend is not collecting - drop the oldest audio rather than grow
    // without bound, and keep the slot accounting straight.
    const size_t overflow = queue_.size() + stereo_samples - kMaxQueuedSamples;
    const size_t drop = std::min(overflow, queue_.size());
    queue_.erase(queue_.begin(), queue_.begin() + drop);
  }
  for (size_t i = 0; i < stereo_samples; ++i) {
    queue_.push_back(FloatToPcm16(stereo[i] * gain));
  }
  ++unreleased_frames_;
}

size_t LibretroAudioDriver::QueuedFrames() {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size() / kOutputChannels;
}

size_t LibretroAudioDriver::Drain(int16_t* out, size_t max_frames) {
  size_t released = 0;
  size_t frames = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    frames = std::min(max_frames, queue_.size() / kOutputChannels);
    const size_t samples = frames * kOutputChannels;
    for (size_t i = 0; i < samples; ++i) {
      // Summed, not overwritten: several drivers can be playing at once (the
      // title's own mixer plus the media player).
      const int32_t mixed = int32_t(out[i]) + int32_t(queue_[i]);
      out[i] = static_cast<int16_t>(std::clamp(mixed, -32768, 32767));
    }
    queue_.erase(queue_.begin(), queue_.begin() + samples);

    // Hand back one slot per whole submitted frame that has now been played.
    if (samples_per_frame_) {
      const size_t still_pending =
          (queue_.size() + samples_per_frame_ - 1) / samples_per_frame_;
      if (unreleased_frames_ > still_pending) {
        released = unreleased_frames_ - still_pending;
        unreleased_frames_ = still_pending;
      }
    }
  }
  if (released && semaphore_) {
    semaphore_->Release(static_cast<int>(released), nullptr);
  }
  return frames;
}

size_t DrainAudioDrivers(int16_t* out, size_t max_frames) {
  std::vector<LibretroAudioDriver*> drivers;
  {
    std::lock_guard<std::mutex> lock(g_drivers_mutex);
    drivers = g_drivers;
  }
  size_t produced = 0;
  for (LibretroAudioDriver* driver : drivers) {
    produced = std::max(produced, driver->Drain(out, max_frames));
  }
  return produced;
}

LibretroAudioSystem::LibretroAudioSystem(cpu::Processor* processor)
    : apu::AudioSystem(processor) {}

LibretroAudioSystem::~LibretroAudioSystem() = default;

std::unique_ptr<apu::AudioSystem> LibretroAudioSystem::Create(
    cpu::Processor* processor) {
  return std::make_unique<LibretroAudioSystem>(processor);
}

X_STATUS LibretroAudioSystem::CreateDriver(size_t index,
                                           threading::Semaphore* semaphore,
                                           apu::AudioDriver** out_driver) {
  auto driver = new LibretroAudioDriver(
      semaphore, apu::AudioDriver::kFrameFrequencyDefault,
      apu::AudioDriver::kFrameChannelsDefault, true);
  if (!driver->Initialize()) {
    delete driver;
    return X_STATUS_UNSUCCESSFUL;
  }
  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

apu::AudioDriver* LibretroAudioSystem::CreateDriver(
    threading::Semaphore* semaphore, uint32_t frequency, uint32_t channels,
    bool need_format_conversion) {
  auto driver = new LibretroAudioDriver(semaphore, frequency, channels,
                                        need_format_conversion);
  if (!driver->Initialize()) {
    delete driver;
    return nullptr;
  }
  return driver;
}

void LibretroAudioSystem::DestroyDriver(apu::AudioDriver* driver) {
  if (!driver) {
    return;
  }
  driver->Shutdown();
  delete driver;
}

}  // namespace libretro
}  // namespace xe
