cmake_minimum_required(VERSION 3.16)
project(winamp_crossfade VERSION 0.2.0 LANGUAGES CXX)
if(WIN32)
  enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Link the C/C++ runtime statically so the plugin is a single self-contained DLL.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra)
endif()
# 32-bit GCC defaults to x87 math; use SSE2 like MSVC does (any PC that runs
# Winamp 5.x has it), for speed and bit-identical results.
if(MINGW AND CMAKE_SIZEOF_VOID_P EQUAL 4)
  add_compile_options(-msse2 -mfpmath=sse)
endif()

option(XF_BUILD_TESTS "Build unit tests" ON)
option(XF_BUILD_TOOLS "Build the offline crossfade simulator" ON)

# --- Portable audio core -----------------------------------------------------
add_library(xfade_core STATIC
  src/core/analysis.cpp
  src/core/engine.cpp
  src/core/fade.cpp
  src/core/pcm.cpp
  src/core/resampler.cpp
  src/core/settings.cpp
)
target_include_directories(xfade_core PUBLIC src/core)
target_compile_definitions(xfade_core PUBLIC XF_VERSION="${PROJECT_VERSION}")
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(xfade_core PUBLIC Threads::Threads)
endif()

# --- Winamp output plugin (Windows only) --------------------------------------
if(WIN32)
  add_library(out_crossfade SHARED
    src/win/plugin.cpp
    src/win/wasapi_output.cpp
    src/win/config_dialog.cpp
    src/win/settings_ini.cpp
    src/win/out_crossfade.rc
  )
  set_target_properties(out_crossfade PROPERTIES PREFIX "" OUTPUT_NAME "out_crossfade")
  target_compile_definitions(out_crossfade PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    XF_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    XF_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    XF_VERSION_PATCH=${PROJECT_VERSION_PATCH})
  target_link_libraries(out_crossfade PRIVATE xfade_core ole32 user32)
  if(MINGW)
    target_link_options(out_crossfade PRIVATE -static -static-libgcc -static-libstdc++ $<$<CONFIG:Release>:-s>)
  endif()
endif()

# --- Tests & tools ------------------------------------------------------------
if(XF_BUILD_TESTS)
  enable_testing()
  add_executable(xfade_tests
    tests/test_main.cpp
    tests/test_analysis.cpp
    tests/test_pcm.cpp
    tests/test_fade.cpp
    tests/test_resampler.cpp
    tests/test_settings.cpp
    tests/test_engine.cpp
  )
  target_link_libraries(xfade_tests PRIVATE xfade_core)
  if(MINGW)
    target_link_options(xfade_tests PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
  add_test(NAME xfade_tests COMMAND xfade_tests)
endif()

if(XF_BUILD_TOOLS)
  add_executable(xfade_sim tools/xfade_sim.cpp tools/wav.cpp)
  target_link_libraries(xfade_sim PRIVATE xfade_core)
  if(MINGW)
    target_link_options(xfade_sim PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
  if(WIN32)
    add_executable(plugin_host tools/plugin_host.cpp)
    if(MINGW)
      target_link_options(plugin_host PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
  endif()
endif()
