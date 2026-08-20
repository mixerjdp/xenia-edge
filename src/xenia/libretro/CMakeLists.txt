# libretro core: xenia loaded as a shared library by a libretro frontend.
# Deliberately links none of the wx/UI-app targets — the frontend owns the
# window, and the emulator runs headless behind it.
add_library(xenia-libretro SHARED)

target_sources(xenia-libretro PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/xenia_libretro.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/libretro_audio_driver.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/libretro_input_driver.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/libretro_log_sink.cc
)

set_target_properties(xenia-libretro PROPERTIES
  OUTPUT_NAME "xenia_libretro"
  PREFIX ""
)

target_compile_definitions(xenia-libretro PRIVATE
  XBYAK_NO_OP_NAMES
  XBYAK_ENABLE_OMITTED_OPERAND
)

target_link_libraries(xenia-libretro PRIVATE
  xenia-apu
  xenia-apu-nop
  xenia-base
  xenia-core
  xenia-cpu
  xenia-gpu
  xenia-gpu-null
  xenia-hid
  xenia-hid-nop
  xenia-kernel
  xenia-patcher
  xenia-ui
  xenia-vfs
  aes_128
  capstone
  fmt
  dxbc
  imgui
  libavcodec
  libavutil
  mspack
  snappy
  xxhash
)

if(XE_TARGET_X86_64)
  target_link_libraries(xenia-libretro PRIVATE xenia-cpu-backend-x64)
elseif(XE_TARGET_AARCH64)
  target_link_libraries(xenia-libretro PRIVATE xenia-cpu-backend-a64)
endif()

# Guest rendering backends. The core reads the finished frame back to system
# memory and hands it to the frontend, so these are xenia's own host devices -
# it never shares a device with the frontend (yet).
if(NOT APPLE)
  target_link_libraries(xenia-libretro PRIVATE
    xenia-gpu-vulkan
    xenia-ui-vulkan
    glslang-spirv
  )
endif()

if(WIN32)
  target_link_libraries(xenia-libretro PRIVATE
    xenia-gpu-d3d12
    xenia-ui-d3d12
    dbghelp
  )
endif()

xe_target_defaults(xenia-libretro)

# Land the core where a frontend expects to find it, next to a matching .info.
set_target_properties(xenia-libretro PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/libretro"
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/libretro"
)
add_custom_command(TARGET xenia-libretro POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/xenia_libretro.info"
          "${PROJECT_BINARY_DIR}/libretro/xenia_libretro.info"
  VERBATIM)
