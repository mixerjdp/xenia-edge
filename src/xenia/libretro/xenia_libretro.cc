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
#include "xenia/libretro/libretro_vulkan.h"
#include "xenia/ui/vulkan/vulkan_device.h"
#include "xenia/ui/vulkan/vulkan_instance.h"
#include "xenia/ui/vulkan/vulkan_presenter.h"
#include "xenia/ui/vulkan/vulkan_provider.h"

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
DECLARE_string(readback_resolve);
DECLARE_bool(disable_context_promotion);
DECLARE_path(d3d12_runtime_dir);
DECLARE_int32(draw_resolution_scale_x);
DECLARE_int32(draw_resolution_scale_y);
DECLARE_uint32(internal_display_resolution);

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
// Resolution scaling multiplies the guest output past the unscaled ceiling, so
// the frontend has to be told the scaled maximum or the frame gets cropped.
unsigned ScaledMaxWidth();
unsigned ScaledMaxHeight();
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

// Deliberately a raw pointer that is never deleted, and a detached thread with
// no object left behind. Xenia has no complete teardown path, and letting a
// static destructor run at DLL_PROCESS_DETACH is worse than leaking: it tears
// down Vulkan and waits on emulator threads while the loader lock is held and
// the process is already exiting, which crashed the frontend on close.
xe::Emulator* g_emulator = nullptr;
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

// Shared Vulkan context. Populated during the frontend's context negotiation,
// which happens inside retro_load_game, long before the emulator thread wants
// a graphics system.
// Fixed for the lifetime of the core: the frontend sizes its framebuffer from
// the maximum reported in retro_get_system_av_info, and that is asked once.
unsigned g_resolution_scale = 1;

bool g_hw_render_requested = false;
retro_hw_render_callback g_hw_render = {};
const retro_hw_render_interface_vulkan* g_vulkan_interface = nullptr;
std::unique_ptr<xe::ui::vulkan::VulkanProvider> g_negotiated_provider;

std::filesystem::path g_storage_root;
std::filesystem::path g_content_root;
std::filesystem::path g_cache_root;

// ---------------------------------------------------------------------------
// Core options
// ---------------------------------------------------------------------------

// Options are declared twice on purpose. The v2 API gives each setting a
// short name and a separate explanation, which is what the frontend's menu
// wants; the legacy list is the fallback for frontends that predate it, where
// the one string has to carry both.
const retro_core_option_v2_definition kCoreOptionDefinitions[] = {
    {"xenia_log_level",
     "Log Detail",
     nullptr,
     "How much of the emulator's log is sent to the frontend. Lower levels "
     "cost less while a game runs.",
     nullptr,
     nullptr,
     {{"info", nullptr},
      {"debug", nullptr},
      {"warning", nullptr},
      {"error", nullptr},
      {"disabled", nullptr},
      {nullptr, nullptr}},
     "info"},
    {"xenia_gpu",
     "Graphics API",
     nullptr,
     "Which API the guest renders through. Both reach the same GPU; 'null' "
     "draws nothing and is only useful for debugging startup. Read once at "
     "startup.",
     nullptr,
     nullptr,
     {{"auto", "Auto (Vulkan)"},
      {"vulkan", "Vulkan"},
      {"d3d12", "Direct3D 12"},
      {"null", "None (debug)"},
      {nullptr, nullptr}},
     "auto"},
    {"xenia_resolution_scale",
     "Internal Resolution",
     nullptr,
     "Renders the guest at a multiple of its native resolution. Sharper, but "
     "costs a lot of GPU time and memory. Read once at startup.",
     nullptr,
     nullptr,
     {{"1x", "1x (native)"},
      {"2x", "2x"},
      {"3x", "3x"},
      {nullptr, nullptr}},
     "1x"},
    {"xenia_display_resolution",
     "Guest Display Resolution",
     nullptr,
     "The resolution the guest believes its display is, for games that honour "
     "it. Unlike the scale below it is a real resolution rather than a "
     "multiplier, so it offers steps in between - but it only works on titles "
     "that support the mode. Read once at startup.",
     nullptr,
     nullptr,
     {{"default", "Default (1280x720)"},
      {"1280x720", "1280x720"},
      {"1280x960", "1280x960"},
      {"1360x768", "1360x768"},
      {"1440x900", "1440x900"},
      {"1680x1050", "1680x1050"},
      {"1920x1080", "1920x1080"},
      {nullptr, nullptr}},
     "default"},
    {"xenia_readback_resolve",
     "Readback Resolve",
     nullptr,
     "Copies rendered surfaces back into guest memory. Games that read their "
     "own framebuffer need 'all' or they come out dark or missing effects. "
     "A per-game config overrides this.",
     nullptr,
     nullptr,
     {{"auto", "Auto (leave as configured)"},
      {"fast", "Fast (only what is read)"},
      {"all", "All (slower, most compatible)"},
      {"none", "None (fastest)"},
      {nullptr, nullptr}},
     "auto"},
    {"xenia_context_promotion",
     "Context Promotion",
     nullptr,
     "Speeds up translated guest code. A few titles, mostly sports games, "
     "animate wrong with it on. A per-game config overrides this.",
     nullptr,
     nullptr,
     {{"auto", "Auto (leave as configured)"},
      {"enabled", "On (faster)"},
      {"disabled", "Off (more compatible)"},
      {nullptr, nullptr}},
     "auto"},
    {"xenia_hw_render",
     "Share GPU Device With Frontend",
     nullptr,
     "Renders on the frontend's own Vulkan device and hands the finished "
     "image over directly, instead of copying every frame through system "
     "memory. Needs the frontend's video driver to be Vulkan, and overrides "
     "the Graphics API setting. Experimental. Read once at startup.",
     nullptr,
     nullptr,
     {{"off", nullptr}, {"on", nullptr}, {nullptr, nullptr}},
     "off"},
    {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {{nullptr, nullptr}},
     nullptr},
};

const retro_core_options_v2 kCoreOptionsV2 = {nullptr,
                                              const_cast<
                                                  retro_core_option_v2_definition*>(
                                                  kCoreOptionDefinitions)};

// Fallback for frontends without the v2 API: one string carrying both the
// name and the choices.
const retro_variable kCoreOptions[] = {
    {"xenia_log_level",
     "Log detail sent to the frontend; info|debug|warning|error|disabled"},
    {"xenia_gpu",
     "Graphics API the guest renders with (restart); auto|vulkan|d3d12|null"},
    {"xenia_resolution_scale",
     "Internal render resolution - sharper, heavier (restart); 1x|2x|3x"},
    {"xenia_display_resolution",
     "Guest display resolution (restart); "
     "default|1280x720|1280x960|1360x768|1440x900|1680x1050|1920x1080"},
    {"xenia_readback_resolve",
     "Readback resolve - fixes dark or missing visuals; auto|fast|all|none"},
    {"xenia_context_promotion",
     "Context promotion - off fixes some sports animations; "
     "auto|enabled|disabled"},
    {"xenia_hw_render",
     "Share the frontend's GPU device (Vulkan only, restart); off|on"},
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

// Xenia clamps to what the device can actually do (scaling needs sparse
// binding on Vulkan or tiled resources on D3D12), so an ambitious value here
// degrades rather than fails. Capped at 3x because the software path has to
// allocate a CPU framebuffer of the full scaled size.
unsigned ScaledMaxWidth() { return kMaxWidth * g_resolution_scale; }
unsigned ScaledMaxHeight() { return kMaxHeight * g_resolution_scale; }

unsigned ReadResolutionScaleOption() {
  const char* value = GetOptionValue("xenia_resolution_scale");
  if (!value) {
    return 1;
  }
  const int parsed = std::atoi(value);
  return unsigned(std::clamp(parsed, 1, 3));
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

// Splits Xenia's roots the way libretro means its directories to be used.
//
// The save directory is for what a user would back up, and that is only the
// guest's saves and profiles - Xenia calls that the content root. Everything
// else is regenerable bulk and belongs in the system directory: the config,
// the shader cache, and the guest's own CACHE and SCRATCH drives, which titles
// fill with installed disc data. Dead or Alive 4 alone parked 1.5 GB there, so
// keeping it out of the save directory matters.
void ResolveRoots() {
  const char* save_dir = nullptr;
  const char* system_dir = nullptr;
  if (g_environ_cb) {
    g_environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir);
    g_environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir);
  }

  // Either directory can be missing, so each falls back to the other and then
  // to the working directory rather than ending up empty.
  std::filesystem::path system_base;
  if (system_dir && system_dir[0]) {
    system_base = std::filesystem::path(system_dir) / "xenia";
  } else if (save_dir && save_dir[0]) {
    system_base = std::filesystem::path(save_dir) / "xenia";
  } else {
    system_base = std::filesystem::current_path() / "xenia";
  }

  std::filesystem::path save_base;
  if (save_dir && save_dir[0]) {
    save_base = std::filesystem::path(save_dir) / "xenia";
  } else {
    save_base = system_base / "content";
  }

  g_storage_root = std::filesystem::absolute(system_base);
  g_content_root = std::filesystem::absolute(save_base);
  // Xenia's host-side cache. Named apart from the guest's CACHE drive, which
  // the emulator hardcodes to <storage root>/cache.
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

// ---------------------------------------------------------------------------
// Shared Vulkan context
// ---------------------------------------------------------------------------

// The frontend's context exists from here until context_destroy. Xenia's own
// objects are built later, on the emulator thread, so this only records that
// the interface is available.
void OnHwContextReset() {
  const retro_hw_render_interface* interface_base = nullptr;
  if (g_environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &interface_base) &&
      interface_base &&
      interface_base->interface_type == RETRO_HW_RENDER_INTERFACE_VULKAN) {
    g_vulkan_interface =
        reinterpret_cast<const retro_hw_render_interface_vulkan*>(
            interface_base);
    g_log_cb(RETRO_LOG_INFO,
             "[xenia] Vulkan render interface acquired (version %u)\n",
             g_vulkan_interface->interface_version);
  } else {
    g_log_cb(RETRO_LOG_ERROR,
             "[xenia] The frontend did not hand over a Vulkan render interface\n");
  }
}

void OnHwContextDestroy() { g_vulkan_interface = nullptr; }

// The frontend creates the instance, so it has to be told up front which API
// version we need. Xenia treats 1.1 as the floor - below it,
// KHR_get_physical_device_properties2 is an extension rather than core, and
// the device feature queries depend on it.
const VkApplicationInfo* GetVulkanApplicationInfo() {
  static const VkApplicationInfo info = {
      VK_STRUCTURE_TYPE_APPLICATION_INFO,
      nullptr,
      "Xenia",
      0,
      "Xenia",
      0,
      VK_MAKE_API_VERSION(0, 1, 1, 0),
  };
  return &info;
}

// Called by the frontend during its video setup. It owns the instance; we
// create the logical device, because only Xenia knows which extensions and
// features its renderer needs, and hand the result back for the frontend to
// share.
bool CreateVulkanDevice(retro_vulkan_context* context, VkInstance instance,
                        VkPhysicalDevice gpu, VkSurfaceKHR surface,
                        PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                        const char** required_device_extensions,
                        unsigned num_required_device_extensions,
                        const char** required_device_layers,
                        unsigned num_required_device_layers,
                        const VkPhysicalDeviceFeatures* required_features) {
  (void)surface;
  (void)required_device_extensions;
  (void)num_required_device_extensions;
  (void)required_device_layers;
  (void)num_required_device_layers;
  (void)required_features;

  xe::ui::vulkan::VulkanInstance::Extensions instance_extensions;
  // At 1.1 this one is core rather than an extension, and the frontend always
  // enables the surface extensions for its own swapchain.
  instance_extensions.ext_1_1_KHR_get_physical_device_properties2 = true;
  instance_extensions.ext_KHR_surface = true;
#ifdef VK_USE_PLATFORM_WIN32_KHR
  instance_extensions.ext_KHR_win32_surface = true;
#endif

  auto adopted_instance = xe::ui::vulkan::VulkanInstance::Adopt(
      instance, get_instance_proc_addr, VK_MAKE_API_VERSION(0, 1, 1, 0),
      instance_extensions);
  if (!adopted_instance) {
    g_log_cb(RETRO_LOG_ERROR, "[xenia] Could not adopt the frontend's Vulkan instance\n");
    return false;
  }

  // Honour the frontend's choice of GPU when it made one; otherwise take the
  // first device Xenia can actually run on.
  std::vector<VkPhysicalDevice> physical_devices;
  if (gpu != VK_NULL_HANDLE) {
    physical_devices.push_back(gpu);
  } else {
    adopted_instance->EnumeratePhysicalDevices(physical_devices);
  }

  std::unique_ptr<xe::ui::vulkan::VulkanDevice> device;
  for (const VkPhysicalDevice physical_device : physical_devices) {
    device = xe::ui::vulkan::VulkanDevice::CreateIfSupported(
        adopted_instance.get(), physical_device, /*with_gpu_emulation=*/true,
        /*with_swapchain=*/true);
    if (device) {
      break;
    }
  }
  if (!device) {
    g_log_cb(RETRO_LOG_ERROR,
             "[xenia] No Vulkan device the emulator can use\n");
    return false;
  }

  // Hand over the graphics/compute family, the one Xenia actually renders on -
  // sharing any other would leave the frontend submitting to a queue that
  // never sees the guest's work.
  const uint32_t queue_family = device->queue_family_graphics_compute();
  const auto& queue_families = device->queue_families();
  if (queue_family >= queue_families.size() ||
      queue_families[queue_family].queues.empty()) {
    g_log_cb(RETRO_LOG_ERROR,
             "[xenia] The Vulkan device exposes no graphics queue\n");
    return false;
  }

  context->gpu = device->physical_device();
  context->device = device->device();
  context->queue = queue_families[queue_family].queues[0]->queue;
  context->queue_family_index = queue_family;
  context->presentation_queue = context->queue;
  context->presentation_queue_family_index = context->queue_family_index;

  g_negotiated_provider = xe::ui::vulkan::VulkanProvider::Adopt(
      std::move(adopted_instance), std::move(device),
      /*with_presentation=*/true);
  if (!g_negotiated_provider) {
    g_log_cb(RETRO_LOG_ERROR, "[xenia] Could not build a provider from the shared device\n");
    return false;
  }

  g_log_cb(RETRO_LOG_INFO,
           "[xenia] Sharing the frontend's Vulkan device (queue family %u)\n",
           context->queue_family_index);
  return true;
}

void DestroyVulkanDevice() {
  // The frontend destroys the instance; our device object goes with the
  // provider, whose lifetime the emulator owns.
  g_negotiated_provider.reset();
}

const retro_hw_render_context_negotiation_interface_vulkan
    kVulkanNegotiationInterface = {
        RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
        RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION,
        GetVulkanApplicationInfo,
        CreateVulkanDevice,
        DestroyVulkanDevice,
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

// Uses the device negotiated with the frontend instead of creating one. The
// provider is built during negotiation, which happens well before the emulator
// thread asks for a graphics system, so it just gets handed over here.
class SharedVulkanGraphicsSystem final
    : public xe::gpu::vulkan::VulkanGraphicsSystem {
 public:
  explicit SharedVulkanGraphicsSystem(
      std::unique_ptr<xe::ui::vulkan::VulkanProvider> provider)
      : provider_to_adopt_(std::move(provider)) {}

  xe::X_STATUS Setup(xe::cpu::Processor* processor,
                     xe::kernel::KernelState* kernel_state,
                     xe::ui::WindowedAppContext* app_context,
                     bool with_presentation) override {
    provider_ = std::move(provider_to_adopt_);
    return xe::gpu::GraphicsSystem::Setup(processor, kernel_state, app_context,
                                          with_presentation);
  }

 private:
  std::unique_ptr<xe::ui::vulkan::VulkanProvider> provider_to_adopt_;
};

std::unique_ptr<xe::gpu::GraphicsSystem> CreateGraphicsSystem() {
  if (g_negotiated_provider) {
    g_log_cb(RETRO_LOG_INFO,
             "[xenia] GPU backend: vulkan (shared with the frontend)\n");
    return std::make_unique<SharedVulkanGraphicsSystem>(
        std::move(g_negotiated_provider));
  }

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
// Knobs a title may need, exposed because editing a per-game TOML by hand is a
// poor way to discover them. FIFA 17 wants both: it reads its own framebuffer
// back for gamma and comes out dark without "all", and its player animations
// misbehave unless context promotion is off.
//
// These are applied *before* the per-game config so that config still wins.
// The frontend persists whatever value its menu last showed, so treating the
// option as authoritative would let a stale "enabled" silently undo a title's
// own override - which is exactly what happened to FIFA.
void ApplyTunableOptions() {
  cvars::draw_resolution_scale_x = int32_t(g_resolution_scale);
  cvars::draw_resolution_scale_y = int32_t(g_resolution_scale);

  // Indices into internal_display_resolution_entries in graphics_system.h.
  if (const char* resolution = GetOptionValue("xenia_display_resolution")) {
    const std::string value(resolution);
    if (value == "1280x720") {
      cvars::internal_display_resolution = 8;
    } else if (value == "1280x960") {
      cvars::internal_display_resolution = 10;
    } else if (value == "1360x768") {
      cvars::internal_display_resolution = 12;
    } else if (value == "1440x900") {
      cvars::internal_display_resolution = 13;
    } else if (value == "1680x1050") {
      cvars::internal_display_resolution = 14;
    } else if (value == "1920x1080") {
      cvars::internal_display_resolution = 16;
    }
  }

  if (const char* mode = GetOptionValue("xenia_readback_resolve")) {
    const std::string value(mode);
    if (value == "fast" || value == "all" || value == "none") {
      cvars::readback_resolve = value;
    }
  }
  if (const char* promotion = GetOptionValue("xenia_context_promotion")) {
    const std::string value(promotion);
    if (value == "enabled") {
      cvars::disable_context_promotion = false;
    } else if (value == "disabled") {
      cvars::disable_context_promotion = true;
    }
  }
}

// These have to win over a per-game config: the core has no plumbing for a
// backend it did not build, and a dialog it cannot draw hangs the frontend.
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
  // ...which leaves the D3D12 backend needing its runtime already in place.
  // Xenia looks next to the host executable by default, and that is the
  // frontend's directory, not ours to fill. Point it at our own data dir so
  // dxcompiler.dll, dxil.dll and D3D12Core.dll live where a libretro core is
  // supposed to keep its files.
  cvars::d3d12_runtime_dir = g_storage_root / "D3D12";

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

  ApplyTunableOptions();
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
  geometry.max_width = ScaledMaxWidth();
  geometry.max_height = ScaledMaxHeight();
  geometry.aspect_ratio = 16.0f / 9.0f;
  g_environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
  g_log_cb(RETRO_LOG_INFO, "[xenia] Guest output is now %ux%u\n", width,
           height);
}

// Hands the guest's own image straight to the frontend, which samples it
// where it already lives on the shared device. No readback, no copy, no
// channel conversion - the whole point of sharing the device.
bool PresentSharedGuestFrame() {
  if (!g_vulkan_interface || g_state.load() != CoreState::kRunning ||
      !g_emulator) {
    return false;
  }
  xe::gpu::GraphicsSystem* graphics_system = g_emulator->graphics_system();
  if (!graphics_system) {
    return false;
  }
  auto* presenter = static_cast<xe::ui::vulkan::VulkanPresenter*>(
      graphics_system->presenter());
  if (!presenter) {
    return false;
  }

  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkExtent2D extent = {};
  if (!presenter->AcquireGuestOutputForSharing(image, view, extent) ||
      !extent.width || !extent.height) {
    return false;
  }

  // The frontend wants the view described as well as handed over, so it can
  // build its own descriptors against it.
  retro_vulkan_image frontend_image = {};
  frontend_image.image_view = view;
  frontend_image.image_layout =
      xe::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
  VkImageViewCreateInfo& view_info = frontend_image.create_info;
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = xe::ui::vulkan::VulkanPresenter::kGuestOutputFormat;
  view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.layerCount = 1;

  // The guest's work is already submitted on the queue both sides share, so
  // the frontend's own submission is ordered after it without a semaphore.
  g_vulkan_interface->set_image(g_vulkan_interface->handle, &frontend_image, 0,
                               nullptr, VK_QUEUE_FAMILY_IGNORED);

  UpdateGeometry(extent.width, extent.height);
  g_video_cb(RETRO_HW_FRAME_BUFFER_VALID, extent.width, extent.height, 0);
  return true;
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

  const unsigned width = std::min<unsigned>(image.width, ScaledMaxWidth());
  const unsigned height = std::min<unsigned>(image.height, ScaledMaxHeight());
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
  // Prefer the v2 declarations so the menu shows a short name with the
  // explanation underneath, and fall back to the flat list otherwise.
  unsigned options_version = 0;
  if (cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &options_version) &&
      options_version >= 2) {
    cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2,
       const_cast<retro_core_options_v2*>(&kCoreOptionsV2));
  } else {
    cb(RETRO_ENVIRONMENT_SET_VARIABLES,
       const_cast<retro_variable*>(kCoreOptions));
  }
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

  // Read before anything sizes itself from it - the frontend asks for the
  // geometry once and cannot be told a bigger maximum later.
  g_resolution_scale = ReadResolutionScaleOption();
  if (g_resolution_scale > 1) {
    g_log_cb(RETRO_LOG_INFO, "[xenia] Internal resolution scale: %ux\n",
             g_resolution_scale);
  }

  // Sized for the maximum the guest can ask for, so a resolution change never
  // has to reallocate on the frontend's thread.
  g_framebuffer.assign(size_t(ScaledMaxWidth()) * ScaledMaxHeight(), 0u);

  g_can_dupe = false;
  g_environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &g_can_dupe);
  g_input_bitmask_supported =
      g_environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, nullptr);

  // Ask for a shared Vulkan context if the user opted in. The frontend calls
  // back into CreateVulkanDevice during this, so the provider exists by the
  // time the emulator thread needs it.
  const char* hw_render = GetOptionValue("xenia_hw_render");
  g_hw_render_requested = hw_render && std::string(hw_render) == "on";
  if (g_hw_render_requested) {
    // Worth saying out loud: when the device is shared, its API is whatever
    // the frontend's video driver uses, so the backend choice has no effect.
    const std::string backend = SelectedGpuBackend();
    if (backend != "vulkan") {
      g_log_cb(RETRO_LOG_WARN,
               "[xenia] Sharing the frontend's device, so the '%s' GPU backend "
               "option is ignored\n",
               backend.c_str());
    }
  }
  if (g_hw_render_requested) {
    if (!g_environ_cb(
            RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE,
            const_cast<retro_hw_render_context_negotiation_interface_vulkan*>(
                &kVulkanNegotiationInterface))) {
      g_log_cb(RETRO_LOG_WARN,
               "[xenia] The frontend refused the Vulkan negotiation interface\n");
      g_hw_render_requested = false;
    }
  }
  if (g_hw_render_requested) {
    g_hw_render.context_type = RETRO_HW_CONTEXT_VULKAN;
    g_hw_render.version_major = 1;
    g_hw_render.version_minor = 1;
    g_hw_render.context_reset = OnHwContextReset;
    g_hw_render.context_destroy = OnHwContextDestroy;
    g_hw_render.cache_context = true;
    g_hw_render.bottom_left_origin = false;
    if (!g_environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &g_hw_render)) {
      g_log_cb(RETRO_LOG_WARN,
               "[xenia] The frontend refused a Vulkan hardware context; "
               "falling back to the software readback\n");
      g_hw_render_requested = false;
    }
  }

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

  g_log_cb(RETRO_LOG_INFO, "[xenia] System root: %s\n",
           xe::path_to_utf8(g_storage_root).c_str());
  g_log_cb(RETRO_LOG_INFO, "[xenia] Save root: %s\n",
           xe::path_to_utf8(g_content_root).c_str());
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
  info->geometry.max_width = ScaledMaxWidth();
  info->geometry.max_height = ScaledMaxHeight();
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

  g_emulator =
      new xe::Emulator("", g_storage_root, g_content_root, g_cache_root);

  // Bare init only — the subsystems come up on the emulator thread, after the
  // per-game config overrides are in place.
  xe::X_STATUS result =
      g_emulator->Setup(nullptr, nullptr, true, CreateAudioSystem,
                        CreateGraphicsSystem, CreateInputDrivers);
  if (XFAILED(result)) {
    Fail(fmt::format("Failed to setup emulator: {:08X}", result));
    // Left dangling on purpose - see the note on g_emulator. Nothing reads it
    // again once the state is kFailed.
    g_emulator = nullptr;
    return false;
  }

  g_state.store(CoreState::kStarting);
  std::thread(EmulatorThread, path).detach();
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

  const bool presented =
      g_hw_render_requested ? PresentSharedGuestFrame() : PresentGuestFrame();
  if (!presented) {
    if (g_state.load() == CoreState::kRunning && g_had_first_frame &&
        g_can_dupe) {
      // Running, just no new guest frame this tick - let the frontend hold the
      // previous one instead of flashing the status screen back up.
      g_video_cb(nullptr, g_output_width, g_output_height,
                 g_output_width * sizeof(uint32_t));
    } else if (!g_hw_render_requested) {
      PaintStatusFrame();
    } else if (g_can_dupe) {
      // Nothing to show yet and no CPU framebuffer allowed under a hardware
      // context, so hold whatever the frontend last had.
      g_video_cb(nullptr, g_output_width, g_output_height, 0);
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
