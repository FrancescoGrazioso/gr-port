cmake_minimum_required(VERSION 3.24)
project(gr_port LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /wd4100)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---- third-party: LibAtrac9 (MIT, external/libatrac9) for NGS2 ATRAC9 voices
file(GLOB ATRAC9_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/external/libatrac9/*.c)
add_library(atrac9 STATIC ${ATRAC9_SOURCES})
target_include_directories(atrac9 PUBLIC ${CMAKE_SOURCE_DIR}/external/libatrac9)
target_compile_definitions(atrac9 PUBLIC COMPILING_DLL)
if(MSVC)
  target_compile_options(atrac9 PRIVATE /W0)
endif()

# ---- shared code -------------------------------------------------------------
add_library(gr_common STATIC
  common/inflate.cpp
  common/sha1.cpp
  common/psarc.cpp
  common/self.cpp
  common/elf_orbis.cpp
  common/nid.cpp
  common/gcn.cpp
)
target_include_directories(gr_common PUBLIC common)

# ---- tools -------------------------------------------------------------------
foreach(tool psarc_tool self2elf nid_inventory gcn_dis)
  add_executable(${tool} tools/${tool}.cpp)
  target_link_libraries(${tool} PRIVATE gr_common)
endforeach()

# ---- loader (Windows only: VirtualAlloc, Win32 window) ----------------------
if(WIN32)
  include(FetchContent)
  # Headers only: the Vulkan loader (vulkan-1.dll) ships with the GPU driver and is
  # loaded at runtime, so no Vulkan SDK installation is required.
  FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.3.296
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(vulkan_headers)

  # x86 disassembler for crash diagnostics and the gr_disasm tool.
  set(CAPSTONE_ARCHITECTURE_DEFAULT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_X86_SUPPORT ON CACHE BOOL "" FORCE)
  set(CAPSTONE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_BUILD_CSTOOL OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(capstone
    GIT_REPOSITORY https://github.com/capstone-engine/capstone.git
    GIT_TAG 5.0.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(capstone)

  # stb_truetype (single header) for libSceFont.
  FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)
  FetchContent_MakeAvailable(stb)

  # glslang: GLSL -> SPIR-V for the GCN shader recompiler (no optimizer, no tools).
  set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
  set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
  set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
  set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
  set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
  set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG 15.4.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(glslang)

  add_executable(gr_disasm tools/gr_disasm.cpp)
  target_link_libraries(gr_disasm PRIVATE gr_common capstone)

  add_executable(gr_port
    loader/main.cpp
    loader/hle.cpp
    loader/hle_functions.cpp
    loader/linker.cpp
    loader/vk_window.cpp
    loader/profiler.cpp
    loader/write_watch.cpp
    loader/tls.cpp
    loader/vmem.cpp
    loader/guest.cpp
    loader/guest_bp.cpp
    loader/kernel/misc.cpp
    loader/kernel/memory.cpp
    loader/kernel/threads.cpp
    loader/kernel/files.cpp
    loader/kernel/libc_internal.cpp
    loader/kernel/sync.cpp
    loader/kernel/services.cpp
    loader/kernel/fiber.cpp
    loader/kernel/audio.cpp
    loader/kernel/pad.cpp
    loader/kernel/avplayer.cpp
    loader/kernel/font.cpp
    loader/kernel/savedata.cpp
    loader/kernel/overlay.cpp
    loader/kernel/audio_out.cpp
    loader/kernel/ngs2.cpp
    loader/gpu/gpu.cpp
    loader/gpu/gnm_driver.cpp
    loader/gpu/videoout.cpp
    loader/gpu/capture.cpp
    loader/gpu/vk_device.cpp
    loader/gpu/shader_recompiler.cpp
    loader/gpu/renderer.cpp
    loader/gpu/renderdoc.cpp
  )
  target_include_directories(gr_port PRIVATE loader ${stb_SOURCE_DIR} external/renderdoc)
  target_link_libraries(gr_port PRIVATE gr_common atrac9 Vulkan::Headers winmm xinput dbghelp mfplat mfreadwrite mfuuid shlwapi ole32
                        glslang glslang-default-resource-limits)
  if(TARGET SPIRV)
    target_link_libraries(gr_port PRIVATE SPIRV)
  endif()
  # Keep symbols in Release too: the crash handler resolves host addresses with DbgHelp.
  target_compile_options(gr_port PRIVATE /Zi)
  target_link_options(gr_port PRIVATE /DEBUG /OPT:REF /OPT:NOICF)
  add_custom_command(TARGET gr_port POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/data/orbis_names.txt $<TARGET_FILE_DIR:gr_port>/orbis_names.txt)
endif()
