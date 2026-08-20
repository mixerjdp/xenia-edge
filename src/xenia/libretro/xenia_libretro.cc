/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// libretro core entry point for xenia.
//
// The emulator runs headless inside the frontend's process: there is no xenia
// window, and the frontend owns the display and the frame clock.
//
// Current scope. The guest renders on xenia's own host GPU device, the finished
// frame is read back to system memory and handed over through video_cb, and
// retro_run drives the guest's vblank so the frontend owns frame pacing. Audio
// is still silence and input is still stubbed; those come next, and after them
// the readback is replaced by sharing the frontend's Vulkan device.

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "xenia/apu/audio_system.h"
#include "xenia/apu/nop/nop_audio_system.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/base/threading.h"
#include "xenia/config.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/null/null_graphics_system.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/ui/presenter.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/nop/nop_hid.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/libretro/libretro.h"
#include "xenia/libretro/libretro_audio_driver.h"
#include "xenia/libretro/libretro_input_driver.h"
#include "xenia/libretro/libretro_log_sink.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/main_win.h"
#include "xenia/gpu/d3d12/d3d12_graphics_system.h"
#endif

DECLARE_string(apu);
DECLARE_string(gpu);
DECLARE_path(log_file);
DECLARE_int32(log_level);
DECLARE_bool(d3d12_install_missing_runtime);
DECLARE_bool(headless);

// The app defines this one in xenia_main.cc, which the core doesn't link.
DEFINE_string(hid, "nop", "Input system. Use: [any, nop, sdl, keyboard]", "HID");

namespace {

using xe::libretro::LibretroLogSink;

// Guest output geometry. The guest picks its own resolution and can change it
// mid-run, so the frontend is told a maximum up front (the cvar validation in
// GraphicsSystem::Setup caps the internal resolution at 1920x1080) and the
// current size is pushed through SET_GEOMETRY whenever it changes.
constexpr unsigned kMaxWidth = 1920;
constexpr unsigned kMaxHeight = 1080;
constexpr unsigned kInitialWidth = 1280;
constexpr unsigned kInitialHeight = 720;
constexpr double kFramesPerSecond = 60.0;
constexpr double kSampleRate = 48000.0;
constexpr size_t kAudioFramesPerVideoFrame =
    static_cast<size_t>(kSampleRate / kFramesPerSecond);

retro_environment_t g_environ_cb = nullptr;
retro_video_refresh_t g_video_cb = nullptr;
retro_audio_sample_batch_t g_audio_batch_cb = nullptr;
retro_input_poll_t g_input_poll_cb = nullptr;
retro_input_state_t g_input_state_cb = nullptr;
retro_log_printf_t g_log_cb = nullptr;

void FallbackLog(enum retro_log_level level, const char* fmt, ...) {
  (void)level;
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
}

// State of the one emulator instance this core owns.
enum class CoreState {
  kIdle,        // no content loaded
  kStarting,    // worker thread is mounting and launching
  kRunning,     // title launched
  kFailed,      // setup or launch failed; message in g_failure_reason
  kSpent,       // content was unloaded; this process can't load again
};

std::unique_ptr<xe::Emulator> g_emulator;
std::thread g_emulator_thread;
std::atomic<CoreState> g_state{CoreState::kIdle};
std::string g_failure_reason;
std::atomic<uint64_t> g_frame_count{0};

std::vector<uint32_t> g_framebuffer;
unsigned g_output_width = kInitialWidth;
unsigned g_output_height = kInitialHeight;
bool g_had_first_frame = false;
std::atomic<bool> g_frontend_ready{false};
bool g_can_dupe = false;
unsigned g_frames_inspected = 0;
bool g_input_bitmask_supported = false;
// One frame's worth of stereo output, refilled every retro_run.
std::vector<int16_t> g_audio_buffer;

std::filesystem::path g_storage_root;
std::filesystem::path g_content_root;
std::filesystem::path g_cache_root;

// ---------------------------------------------------------------------------
// Core options
// ---------------------------------------------------------------------------

// Legacy SET_VARIABLES rather than the v2 option API: every frontend
// understands it, and phase 1 only needs a couple of knobs. Phase 5 upgrades
// this to the full v2 definitions once the option set is worth it.
// The first value after the ';' is the default, so keep info first — a
// work-in-progress core that logs nothing by default is useless to debug.
const retro_variable kCoreOptions[] = {
    {"xenia_log_level",
     "Log level; info|debug|warning|error|disabled"},
    {"xenia_apu",
     "Audio system (restart required); nop|any|xaudio2|sdl"},
    {"xenia_gpu",
     "GPU backend (restart required); auto|vulkan|d3d12|null"},
    {nullptr, nullptr},
};

const char* GetOptionValue(const char* key) {
  retro_variable var = {};
  var.key = key;
  if (g_environ_cb && g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) &&
      var.value) {
    return var.value;
  }
  return nullptr;
}

void ApplyLogLevelOption() {
  const char* value = GetOptionValue("xenia_log_level");
  if (!value) {
    return;
  }
  const std::string level(value);
  if (level == "disabled") {
    cvars::log_level = -1;
  } else if (level == "error") {
    cvars::log_level = 0;
  } else if (level == "warning") {
    cvars::log_level = 1;
  } else if (level == "info") {
    cvars::log_level = 2;
  } else if (level == "debug") {
    cvars::log_level = 3;
  }
}

// ---------------------------------------------------------------------------
// Host directories
// ---------------------------------------------------------------------------

// Resolves where xenia keeps its config, content and shader caches. RetroArch
// gives us a system dir (read-mostly) and a save dir (writable); xenia writes
// to all of its roots, so everything hangs off the save dir when there is one.
void ResolveRoots() {
  const char* save_dir = nullptr;
  const char* system_dir = nullptr;
  if (g_environ_cb) {
    g_environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir);
    g_environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir);
  }
  std::filesystem::path base;
  if (save_dir && save_dir[0]) {
    base = std::filesystem::path(save_dir) / "xenia";
  } else if (system_dir && system_dir[0]) {
    base = std::filesystem::path(system_dir) / "xenia";
  } else {
    base = std::filesystem::current_path() / "xenia";
  }
  g_storage_root = std::filesystem::absolute(base);
  g_content_root = g_storage_root / "content";
  g_cache_root = g_storage_root / "cache_host";

  std::error_code ec;
  std::filesystem::create_directories(g_storage_root, ec);
  std::filesystem::create_directories(g_content_root, ec);
  std::filesystem::create_directories(g_cache_root, ec);
}

// ---------------------------------------------------------------------------
// Subsystem factories
// ---------------------------------------------------------------------------

std::unique_ptr<xe::apu::AudioSystem> CreateAudioSystem(
    xe::cpu::Processor* processor) {
  return xe::libretro::LibretroAudioSystem::Create(processor);
}

// The stock null graphics system is not actually backend-free: its Setup
// creates a Vulkan provider anyway ("we still setup vulkan because UI needs it
// through us"). Creating a second VkInstance inside the frontend's process
// deadlocks against the overlay layers injected into it (RivaTuner, OBS both
// showed up in the hung stack), and phase 1 wants no host GPU at all — so skip
// the provider and go straight to the base Setup, which leaves the presenter
// null when there is no provider.
//
// Phase 4 is where a real Vulkan device appears, and it must come from the
// frontend's context negotiation rather than an instance of our own.
class HeadlessGraphicsSystem final : public xe::gpu::null::NullGraphicsSystem {
 public:
  xe::X_STATUS Setup(xe::cpu::Processor* processor,
                     xe::kernel::KernelState* kernel_state,
                     xe::ui::WindowedAppContext* app_context,
                     bool /*with_presentation*/) override {
    return xe::gpu::GraphicsSystem::Setup(processor, kernel_state, app_context,
                                          false);
  }
};

// Backend the guest renders with. This is xenia's own host GPU device, not the
// frontend's: the frame is read back to system memory and handed over through
// video_cb, so the two never share a device. Phase 4 replaces the readback with
// the frontend's own Vulkan device.
//
// Direct3D 12 is the default on Windows for a concrete reason: bringing up a
// second Vulkan instance in the frontend's process deadlocked against the
// overlay layers injected into it (RivaTuner and OBS). D3D12 sidesteps that
// whole class of conflict.
std::string SelectedGpuBackend() {
  const char* value = GetOptionValue("xenia_gpu");
  std::string name = value ? value : "";
  if (name.empty() || name == "auto") {
    // Vulkan by default even on Windows: xenia's D3D12 backend needs
    // dxcompiler.dll and dxil.dll next to the frontend's executable, which we
    // can't go download behind the user's back. d3d12 stays selectable for
    // anyone who has that runtime in place.
    name = "vulkan";
  }
  return name;
}

std::unique_ptr<xe::gpu::GraphicsSystem> CreateGraphicsSystem() {
  const std::string backend = SelectedGpuBackend();
  g_log_cb(RETRO_LOG_INFO, "[xenia] GPU backend: %s\n", backend.c_str());
#if XE_PLATFORM_WIN32
  if (backend == "d3d12") {
    return std::make_unique<xe::gpu::d3d12::D3D12GraphicsSystem>();
  }
#endif
  if (backend == "vulkan") {
    return std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>();
  }
  return std::make_unique<HeadlessGraphicsSystem>();
}

std::vector<std::unique_ptr<xe::hid::InputDriver>> CreateInputDrivers(
    xe::ui::Window* window) {
  std::vector<std::unique_ptr<xe::hid::InputDriver>> drivers;
  auto driver = std::make_unique<xe::libretro::LibretroInputDriver>(window, 0);
  xe::libretro::SetActiveInputDriver(driver.get());
  drivers.emplace_back(std::move(driver));
  return drivers;
}

// ---------------------------------------------------------------------------
// Emulator lifecycle
// ---------------------------------------------------------------------------

// Reapplied after the per-game config loads so a title override can't select
// a backend the core has no plumbing for. Assigned rather than set through
// OVERRIDE_string, which only expands in the TU that defines the cvar.
void ApplyBackendCvars() {
  cvars::gpu = SelectedGpuBackend();
  // The factories hand back the libretro-backed audio and input systems
  // regardless of these, but the emulator logs and persists the names.
  cvars::apu = "libretro";
  cvars::hid = "libretro";
  // A core must never put a modal dialog in front of the frontend. Xenia's
  // D3D12 provider otherwise offers to download the DXIL shader compiler and
  // blocks on the answer, which wedges the whole frontend.
  cvars::d3d12_install_missing_runtime = false;

  // The guest raises its own dialogs too - the sign-in blade, the on-screen
  // keyboard, storage device pickers - and xenia draws those with ImGui into
  // its own window. There is no window here, so the guest would wait forever
  // for an answer nobody can give: Dead or Alive 4 freezes on Start exactly
  // that way. Headless mode makes each of those take its default instead.
  cvars::headless = true;
}

// Gamertag for the profile the core creates on first run. Titles show it, and
// saves are filed under it, so it wants to be recognisable rather than clever.
constexpr const char* kDefaultGamertag = "Xenia";

// Standalone xenia asks the user to create a profile the first time it runs.
// A core has nobody to ask, and a title that finds no profile can hang looking
// for save containers that can't exist - Dead or Alive 4 spins in
// StfsControlDevice exactly that way. So make sure one profile exists and is
// signed in before the title starts.
void EnsureProfileSignedIn() {
  if (!g_emulator || !g_emulator->kernel_state()) {
    return;
  }
  xe::kernel::xam::XamState* xam_state = g_emulator->kernel_state()->xam_state();
  if (!xam_state) {
    return;
  }
  xe::kernel::xam::ProfileManager* profiles = xam_state->profile_manager();
  if (!profiles) {
    return;
  }

  if (profiles->GetAccountCount() == 0) {
    if (profiles->CreateProfile(kDefaultGamertag, /*autologin=*/true)) {
      g_log_cb(RETRO_LOG_INFO, "[xenia] Created and signed in profile '%s'\n",
               kDefaultGamertag);
    } else {
      g_log_cb(RETRO_LOG_WARN,
               "[xenia] Could not create a default profile; titles that need "
               "one may not boot\n");
    }
    return;
  }

  // Profiles carried over from an earlier run, but nothing signs them in
  // without the UI that normally does it.
  if (!profiles->IsAnyProfileSignedIn()) {
    const auto* accounts = profiles->GetAccounts();
    if (accounts && !accounts->empty()) {
      const uint64_t xuid = accounts->begin()->first;
      profiles->Login(xuid);
      g_log_cb(RETRO_LOG_INFO, "[xenia] Signed in existing profile %016llX\n",
               static_cast<unsigned long long>(xuid));
    }
  }
}

void Fail(const std::string& reason) {
  g_failure_reason = reason;
  g_state.store(CoreState::kFailed);
  g_log_cb(RETRO_LOG_ERROR, "[xenia] %s\n", reason.c_str());
}

// Runs on its own thread, mirroring EmulatorApp::EmulatorThread: everything
// past Setup can block for a long time, and the frontend's thread must stay
// free to keep calling retro_run.
void EmulatorThread(std::filesystem::path path) {
  xe::threading::set_name("Xenia Emulator");

  g_emulator->MountStandardDrives();
  EnsureProfileSignedIn();

  config::LoadGameConfigForFile(path);

  // Wait for the frontend to finish standing up its own video context before
  // creating ours. Racing it is what deadlocked vkCreateInstance against the
  // overlay layers injected into the process, and deferring the heavy startup
  // to the first frame is how the Cemu core sequences this too.
  while (!g_frontend_ready.load(std::memory_order_acquire) &&
         g_state.load() == CoreState::kStarting) {
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
  // After the per-game config, so a title override can't drag in a backend
  // phase 1 has no plumbing for.
  ApplyBackendCvars();

  xe::X_STATUS result = g_emulator->SetupSubsystems();
  if (XFAILED(result)) {
    Fail(fmt::format("Failed to setup subsystems: {:08X}", result));
    return;
  }

  if (xe::gpu::GraphicsSystem* graphics_system =
          g_emulator->graphics_system()) {
    graphics_system->SetHostDrivenVblank(true);
  }

  g_log_cb(RETRO_LOG_INFO, "[xenia] Launching %s\n",
           xe::path_to_utf8(path).c_str());
  result = g_emulator->LaunchPath(path);
  if (XFAILED(result)) {
    Fail(fmt::format("Failed to launch title: {:08X}", result));
    return;
  }

  g_state.store(CoreState::kRunning);
  g_log_cb(RETRO_LOG_INFO, "[xenia] Title running: %s (%08X)\n",
           g_emulator->title_name().c_str(), g_emulator->title_id());

  g_emulator->WaitUntilExit();
  g_log_cb(RETRO_LOG_INFO, "[xenia] Title exited\n");
}

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------

// Paints a flat status color with a sweeping bar, shown until the guest
// produces its first frame so the frontend makes it obvious whether the core
// is booting, running or wedged.
void PaintStatusFrame() {
  uint32_t color;
  switch (g_state.load()) {
    case CoreState::kRunning:
      color = 0x00103018u;  // green: title running, no frame yet
      break;
    case CoreState::kFailed:
      color = 0x00401010u;  // red: setup or launch failed
      break;
    case CoreState::kSpent:
      color = 0x00303030u;  // gray: content unloaded
      break;
    default:
      color = 0x00101838u;  // blue: still coming up
      break;
  }
  const unsigned width = g_output_width;
  const unsigned height = g_output_height;
  std::fill_n(g_framebuffer.begin(), size_t(width) * height, color);

  const uint64_t frame = g_frame_count.load();
  const unsigned bar_x = static_cast<unsigned>((frame * 4) % width);
  const unsigned bar_y = height / 2 - 4;
  for (unsigned y = bar_y; y < bar_y + 8; ++y) {
    for (unsigned x = bar_x; x < bar_x + 16 && x < width; ++x) {
      g_framebuffer[size_t(y) * width + x] = 0x00c0c0c0u;
    }
  }

  g_video_cb(g_framebuffer.data(), width, height, width * sizeof(uint32_t));
}

// Tells the frontend the guest changed resolution. Cheap to call every frame -
// it only does anything when the size actually moved.
void UpdateGeometry(unsigned width, unsigned height) {
  if (width == g_output_width && height == g_output_height) {
    return;
  }
  g_output_width = width;
  g_output_height = height;

  retro_game_geometry geometry = {};
  geometry.base_width = width;
  geometry.base_height = height;
  geometry.max_width = kMaxWidth;
  geometry.max_height = kMaxHeight;
  geometry.aspect_ratio = 16.0f / 9.0f;
  g_environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
  g_log_cb(RETRO_LOG_INFO, "[xenia] Guest output is now %ux%u\n", width,
           height);
}

// Pulls the latest guest frame out of the presenter and hands it to the
// frontend. Returns false when there is nothing to present.
//
// This is the slow-but-correct path: CaptureGuestOutput reads the image back
// from the host GPU into system memory every frame. Phase 4 removes the
// readback by giving the frontend the VkImage directly.
bool PresentGuestFrame() {
  if (g_state.load() != CoreState::kRunning || !g_emulator) {
    return false;
  }
  xe::gpu::GraphicsSystem* graphics_system = g_emulator->graphics_system();
  if (!graphics_system) {
    return false;
  }
  xe::ui::Presenter* presenter = graphics_system->presenter();
  if (!presenter) {
    return false;
  }

  xe::ui::RawImage image;
  if (!presenter->CaptureGuestOutput(image) || !image.width ||
      !image.height || image.data.empty()) {
    return false;
  }

  const unsigned width = std::min<unsigned>(image.width, kMaxWidth);
  const unsigned height = std::min<unsigned>(image.height, kMaxHeight);
  UpdateGeometry(width, height);

  // RawImage is R8 G8 B8 X8 in memory; XRGB8888 wants 0x00RRGGBB, which is
  // B G R X once written as a little-endian word. So the channels have to be
  // reordered rather than just copied.
  for (unsigned y = 0; y < height; ++y) {
    const uint8_t* src = image.data.data() + size_t(y) * image.stride;
    uint32_t* dst = g_framebuffer.data() + size_t(y) * width;
    for (unsigned x = 0; x < width; ++x) {
      dst[x] = (uint32_t(src[0]) << 16) | (uint32_t(src[1]) << 8) |
               uint32_t(src[2]);
      src += 4;
    }
  }

  // Diagnostic for the first handful of captured frames: tells a genuinely
  // black guest screen apart from a capture or channel-order bug.
  if (g_frames_inspected < 10) {
    ++g_frames_inspected;
    uint32_t seen_max = 0;
    size_t non_black = 0;
    const size_t pixels = size_t(width) * height;
    for (size_t i = 0; i < pixels; ++i) {
      const uint32_t px = g_framebuffer[i] & 0x00FFFFFFu;
      seen_max = std::max(seen_max, px);
      non_black += px ? 1 : 0;
    }
    g_log_cb(RETRO_LOG_INFO,
             "[xenia] frame %u: %ux%u stride=%zu non-black=%zu/%zu max=%06X\n",
             unsigned(g_frames_inspected), width, height, image.stride,
             non_black, pixels, seen_max);
  }

  g_video_cb(g_framebuffer.data(), width, height, width * sizeof(uint32_t));
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// libretro API
// ---------------------------------------------------------------------------

extern "C" {

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t cb) {
  g_environ_cb = cb;
  cb(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<retro_variable*>(kCoreOptions));
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) {
  g_video_cb = cb;
}
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
  g_audio_batch_cb = cb;
}
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) {
  g_input_poll_cb = cb;
}
RETRO_API void retro_set_input_state(retro_input_state_t cb) {
  g_input_state_cb = cb;
}

RETRO_API void retro_init(void) {
  retro_log_callback log = {};
  if (g_environ_cb && g_environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log) &&
      log.log) {
    g_log_cb = log.log;
  } else {
    g_log_cb = FallbackLog;
  }

  ResolveRoots();

  // Keep xenia's own log next to its storage rather than in the frontend's
  // install directory, which is what GetExecutableFolder would resolve to.
  cvars::log_file = g_storage_root / "xenia_libretro.log";

  // The sink goes in at init: the logger's writer thread reads the sink list
  // without synchronization, so attaching one later races with it (and did
  // crash the frontend inside FlushAllSinks).
#if XE_PLATFORM_WIN32
  xe::InitializeWin32App("xenia_libretro",
                         std::make_unique<LibretroLogSink>(g_log_cb));
#else
  xe::InitializeLogging("xenia_libretro",
                        std::make_unique<LibretroLogSink>(g_log_cb));
#endif

  ApplyLogLevelOption();

  config::SetupConfig(g_storage_root);

  // Sized for the maximum the guest can ask for, so a resolution change never
  // has to reallocate on the frontend's thread.
  g_framebuffer.assign(size_t(kMaxWidth) * kMaxHeight, 0u);

  g_can_dupe = false;
  g_environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &g_can_dupe);
  g_input_bitmask_supported =
      g_environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, nullptr);

  // Names the guest's buttons in the frontend's remapping UI. Without this it
  // shows the bare RetroPad labels, which do not match an Xbox pad.
  static const retro_controller_description kPadDescription[] = {
      {"Xbox 360 Controller", RETRO_DEVICE_JOYPAD},
  };
  static const retro_controller_info kControllerInfo[] = {
      {kPadDescription, 1}, {kPadDescription, 1},
      {kPadDescription, 1}, {kPadDescription, 1},
      {nullptr, 0},
  };
  g_environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO,
               const_cast<retro_controller_info*>(kControllerInfo));

  static const retro_input_descriptor kInputDescriptors[] = {
#define XE_PAD_PORT(port)                                                       {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "A"},                    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "B"},                {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "X"},                {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Y"},                {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up"},        {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,                    "D-Pad Down"},                                                               {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,                    "D-Pad Left"},                                                               {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,                   "D-Pad Right"},                                                              {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},        {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Back"},        {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "LB"},               {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "RB"},               {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "LT"},              {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "RT"},              {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,                      "Left Stick Click"},                                                         {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,                      "Right Stick Click"},                                                        {port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,                    RETRO_DEVICE_ID_ANALOG_X, "Left Stick X"},                                   {port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,                    RETRO_DEVICE_ID_ANALOG_Y, "Left Stick Y"},                                   {port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,                   RETRO_DEVICE_ID_ANALOG_X, "Right Stick X"},                                  {port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,                   RETRO_DEVICE_ID_ANALOG_Y, "Right Stick Y"}
      XE_PAD_PORT(0), XE_PAD_PORT(1), XE_PAD_PORT(2), XE_PAD_PORT(3),
#undef XE_PAD_PORT
      {0, 0, 0, 0, nullptr},
  };
  g_environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
               const_cast<retro_input_descriptor*>(kInputDescriptors));
  g_audio_buffer.assign(kAudioFramesPerVideoFrame * 2, 0);

  g_log_cb(RETRO_LOG_INFO, "[xenia] Storage root: %s\n",
           xe::path_to_utf8(g_storage_root).c_str());
}

RETRO_API void retro_deinit(void) {
  // Xenia has no complete teardown path (the app itself exits via quick_exit),
  // so tearing the emulator down here would be more likely to crash the
  // frontend than to free anything that matters for the rest of its lifetime.
  g_log_cb(RETRO_LOG_INFO, "[xenia] retro_deinit\n");
}

RETRO_API void retro_get_system_info(struct retro_system_info* info) {
  std::memset(info, 0, sizeof(*info));
  info->library_name = "Xenia-edge";
  info->library_version = "0.1-phase1";
  info->valid_extensions = "iso|xex|zar|xcp";
  info->need_fullpath = true;
  info->block_extract = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info) {
  std::memset(info, 0, sizeof(*info));
  info->geometry.base_width = g_output_width;
  info->geometry.base_height = g_output_height;
  info->geometry.max_width = kMaxWidth;
  info->geometry.max_height = kMaxHeight;
  info->geometry.aspect_ratio = 16.0f / 9.0f;
  info->timing.fps = kFramesPerSecond;
  info->timing.sample_rate = kSampleRate;
}

RETRO_API void retro_set_controller_port_device(unsigned port,
                                                unsigned device) {
  (void)port;
  (void)device;
}

RETRO_API void retro_reset(void) {
  // A guest reset means relaunching the title, which needs the teardown path
  // phase 1 doesn't have yet.
  g_log_cb(RETRO_LOG_WARN, "[xenia] Reset is not implemented yet\n");
}

RETRO_API bool retro_load_game(const struct retro_game_info* game) {
  if (!game || !game->path) {
    g_log_cb(RETRO_LOG_ERROR, "[xenia] No content path given\n");
    return false;
  }
  if (g_state.load() != CoreState::kIdle) {
    g_log_cb(RETRO_LOG_ERROR,
             "[xenia] This core can only load content once per process; "
             "restart the frontend to load another title\n");
    return false;
  }

  enum retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_XRGB8888;
  if (!g_environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pixel_format)) {
    g_log_cb(RETRO_LOG_ERROR, "[xenia] XRGB8888 is not supported\n");
    return false;
  }

  const std::filesystem::path path =
      std::filesystem::absolute(xe::to_path(game->path));

  g_emulator = std::make_unique<xe::Emulator>("", g_storage_root,
                                              g_content_root, g_cache_root);

  // Bare init only — the subsystems come up on the emulator thread, after the
  // per-game config overrides are in place.
  xe::X_STATUS result =
      g_emulator->Setup(nullptr, nullptr, true, CreateAudioSystem,
                        CreateGraphicsSystem, CreateInputDrivers);
  if (XFAILED(result)) {
    Fail(fmt::format("Failed to setup emulator: {:08X}", result));
    g_emulator.reset();
    return false;
  }

  g_state.store(CoreState::kStarting);
  g_emulator_thread = std::thread(EmulatorThread, path);
  return true;
}

RETRO_API bool retro_load_game_special(unsigned game_type,
                                       const struct retro_game_info* info,
                                       size_t num_info) {
  (void)game_type;
  (void)info;
  (void)num_info;
  return false;
}

RETRO_API void retro_unload_game(void) {
  if (g_state.load() == CoreState::kIdle) {
    return;
  }
  // Same reasoning as retro_deinit: the emulator keeps running its own threads
  // and is deliberately left alive rather than torn down into a crash. The
  // frontend has to be restarted before another title can be loaded.
  g_log_cb(RETRO_LOG_WARN,
           "[xenia] Content unloaded, but the emulator cannot be torn down "
           "yet; restart the frontend before loading other content\n");
  if (g_emulator_thread.joinable()) {
    g_emulator_thread.detach();
  }
  g_state.store(CoreState::kSpent);
}

RETRO_API void retro_run(void) {
  g_frame_count.fetch_add(1);
  // The frontend's video context exists by the time it starts calling us, so
  // this is the emulator thread's cue to bring the graphics system up.
  g_frontend_ready.store(true, std::memory_order_release);

  bool options_updated = false;
  if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &options_updated) &&
      options_updated) {
    ApplyLogLevelOption();
  }

  if (g_input_poll_cb) {
    g_input_poll_cb();
    xe::libretro::PollLibretroInput(g_input_state_cb, g_input_bitmask_supported);
  }

  // Frame pacing is inverted compared to standalone xenia: the frontend calls
  // us once per frame, and that call is what advances the guest's vblank. The
  // emulator's own frame limiter thread stands down (SetHostDrivenVblank).
  if (g_emulator) {
    if (xe::gpu::GraphicsSystem* graphics_system =
            g_emulator->graphics_system()) {
      graphics_system->TriggerHostVblank();
    }
  }

  if (!PresentGuestFrame()) {
    if (g_state.load() == CoreState::kRunning && g_had_first_frame &&
        g_can_dupe) {
      // Running, just no new guest frame this tick - let the frontend hold the
      // previous one instead of flashing the status screen back up.
      g_video_cb(nullptr, g_output_width, g_output_height,
                 g_output_width * sizeof(uint32_t));
    } else {
      PaintStatusFrame();
    }
  } else {
    g_had_first_frame = true;
  }

  if (g_audio_batch_cb) {
    // Always hand over a full frame's worth: the drivers fill in what they
    // have and the rest stays silent, which the frontend prefers to a short
    // or missing batch.
    std::fill(g_audio_buffer.begin(), g_audio_buffer.end(), int16_t(0));
    xe::libretro::DrainAudioDrivers(g_audio_buffer.data(),
                                    kAudioFramesPerVideoFrame);
    g_audio_batch_cb(g_audio_buffer.data(), kAudioFramesPerVideoFrame);
  }
}

RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void* data, size_t size) {
  (void)data;
  (void)size;
  return false;
}
RETRO_API bool retro_unserialize(const void* data, size_t size) {
  (void)data;
  (void)size;
  return false;
}

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

RETRO_API void* retro_get_memory_data(unsigned id) {
  (void)id;
  return nullptr;
}
RETRO_API size_t retro_get_memory_size(unsigned id) {
  (void)id;
  return 0;
}

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled,
                               const char* code) {
  (void)index;
  (void)enabled;
  (void)code;
}

}  // extern "C"
