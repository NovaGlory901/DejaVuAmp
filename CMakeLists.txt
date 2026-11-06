cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)     # runtime-dependency paths are matched after normalising them (Windows)
endif()
project(DejaVuAmp VERSION 0.1.0 DESCRIPTION "A player with a 10-band equalizer, C++ port" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DVA_BUILD_TESTS "Build and register the tests" ON)
option(DVA_SANITIZE "Build with the address and undefined-behaviour sanitizers (GCC/Clang)" OFF)

# Must come before any target is made, or the libraries are built without it.
if(DVA_SANITIZE AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# MinGW: build the compiler's own runtime into the program, so fewer DLLs have to travel with it.
if(MINGW)
  add_link_options(-static-libgcc -static-libstdc++)
endif()

# The pieces both native programs use (the JSON reader, UTF-8 paths, the sound analysis, the random picker, folder watching, miniaudio, Dear ImGui)
# live in shared/native/. Nothing here depends on other projects.
set(SHARED ${CMAKE_CURRENT_SOURCE_DIR}/shared/native)

# The logic: settings, equalizer presets, formatters. Uses the shared JSON reader.
add_library(dva_core src/core.cpp src/dsp.cpp src/stretch.cpp src/theme.cpp ${SHARED}/src/json.cpp)
target_include_directories(dva_core PUBLIC include ${SHARED}/include)

# Playing: decoding (miniaudio's MP3/FLAC/WAV decoders, stb_vorbis), tags, the player engine and the sound device (miniaudio).
find_package(Threads REQUIRED)
add_library(dva_play src/decoder.cpp src/tags.cpp src/player.cpp src/output.cpp src/miniaudio_impl.cpp src/vorbis_impl.c)
target_include_directories(dva_play PUBLIC include)
target_include_directories(dva_play SYSTEM PRIVATE ${SHARED}/third_party/miniaudio third_party/stb_vorbis)
target_link_libraries(dva_play PUBLIC dva_core Threads::Threads ${CMAKE_DL_LIBS})
# M4A/AAC/ALAC, Opus, WMA and friends through ffmpeg's libraries, when they are installed (the system's copy: it is kept up to date by the system).
option(DVA_FFMPEG "Play M4A, Opus, WMA and AAC through ffmpeg's libraries (needs libavformat, libavcodec, libswresample, libavutil)" ON)
find_package(PkgConfig QUIET)
if(DVA_FFMPEG AND PkgConfig_FOUND)
  pkg_check_modules(DVA_FFMPEGLIB QUIET IMPORTED_TARGET libavformat libavcodec libswresample libavutil)
endif()
if(DVA_FFMPEGLIB_FOUND)
  target_sources(dva_play PRIVATE src/decoder_ffmpeg.cpp)
  target_compile_definitions(dva_play PUBLIC DVA_HAVE_FFMPEG)
  target_link_libraries(dva_play PUBLIC PkgConfig::DVA_FFMPEGLIB)
  message(STATUS "M4A/AAC/Opus/WMA: supported (ffmpeg, libavformat ${DVA_FFMPEGLIB_libavformat_VERSION})")
else()
  message(STATUS "M4A/AAC/Opus/WMA: not supported (ffmpeg libraries not found, or DVA_FFMPEG is off)")
endif()
# the third-party files are not held to this project's warning level
# -fwrapv: the FLAC decoder's prediction arithmetic overflows on damaged files; with it that is defined (wraps, as the hardware does) instead of undefined
set_source_files_properties(src/miniaudio_impl.cpp src/vorbis_impl.c PROPERTIES COMPILE_OPTIONS "-w;-fwrapv")

foreach(lib dva_core dva_play)
  if(MSVC)
    target_compile_options(${lib} PRIVATE /W4)
  else()
    target_compile_options(${lib} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wcast-align
      $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor> $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>)
  endif()
endforeach()

# The window (SDL2 + OpenGL ES 2 + Dear ImGui, vendored in shared/native/third_party/imgui). Skipped, with a note, if SDL2 or GLES2 are missing.
option(DVA_BUILD_APP "Build the dejavuamp window" ON)
if(DVA_BUILD_APP)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(DVA_SDL2 QUIET IMPORTED_TARGET sdl2)
    pkg_check_modules(DVA_GLES2 QUIET IMPORTED_TARGET glesv2)
  endif()
  # Windows (MSYS2 ANGLE package): the headers and libGLESv2 are there but there is no glesv2.pc, so look for them directly.
  if(NOT DVA_GLES2_FOUND)
    find_path(DVA_GLES2_INCLUDE GLES2/gl2.h)
    find_library(DVA_GLES2_LIB NAMES GLESv2 libGLESv2)
    if(DVA_GLES2_INCLUDE AND DVA_GLES2_LIB)
      add_library(PkgConfig::DVA_GLES2 INTERFACE IMPORTED)
      target_include_directories(PkgConfig::DVA_GLES2 INTERFACE ${DVA_GLES2_INCLUDE})
      target_link_libraries(PkgConfig::DVA_GLES2 INTERFACE ${DVA_GLES2_LIB})
      set(DVA_GLES2_FOUND TRUE)
      message(STATUS "OpenGL ES 2: ${DVA_GLES2_LIB}")
    endif()
  endif()
  if(DVA_SDL2_FOUND AND DVA_GLES2_FOUND)
    add_library(dva_imgui STATIC
      ${SHARED}/third_party/imgui/imgui.cpp ${SHARED}/third_party/imgui/imgui_draw.cpp ${SHARED}/third_party/imgui/imgui_tables.cpp ${SHARED}/third_party/imgui/imgui_widgets.cpp
      ${SHARED}/third_party/imgui/backends/imgui_impl_sdl2.cpp ${SHARED}/third_party/imgui/backends/imgui_impl_opengl3.cpp)
    target_include_directories(dva_imgui SYSTEM PUBLIC ${SHARED}/third_party/imgui)
    target_compile_definitions(dva_imgui PUBLIC IMGUI_IMPL_OPENGL_ES2)
    target_link_libraries(dva_imgui PUBLIC PkgConfig::DVA_SDL2 PkgConfig::DVA_GLES2 ${CMAKE_DL_LIBS})
    # the shared pieces the window reuses: the spectrum analysis, the random picker, folder watching
    add_library(dva_shared ${SHARED}/src/analyser.cpp ${SHARED}/src/audio.cpp ${SHARED}/src/picker.cpp ${SHARED}/src/userfiles.cpp)
    target_include_directories(dva_shared PUBLIC ${SHARED}/include)
    add_executable(dejavuamp apps/dejavuamp.cpp apps/skin.cpp)
    target_link_libraries(dejavuamp PRIVATE dva_play dva_shared dva_imgui)
    if(WIN32)
      set_target_properties(dejavuamp PROPERTIES OUTPUT_NAME DejaVuAmp)     # Windows: DejaVuAmp.exe (Linux keeps `dejavuamp`)
    endif()
    target_compile_definitions(dejavuamp PRIVATE DVA_VERSION="${PROJECT_VERSION}")
    # `cmake --install build-release --prefix ~/DejaVuAmp` makes a folder that runs from anywhere (the program finds its folders next to itself, also through a
    # link in ~/.local/bin): the program, the font, and the plain folders you drop files in (only their README is copied, so your own files are never touched).
    if(WIN32)
      # Windows: the DLLs the program needs (SDL2, ffmpeg's libraries and what they use, ANGLE's libGLESv2, the thread library) go next to it.
      # SDL loads ANGLE's libEGL.dll by name at run time, so the dependency scan cannot see it: it is added by hand.
      install(TARGETS dejavuamp RUNTIME_DEPENDENCY_SET dva_runtime_deps RUNTIME DESTINATION .)
      get_filename_component(DVA_DLL_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)     # MSYS2 keeps its DLLs next to the compiler
      install(RUNTIME_DEPENDENCY_SET dva_runtime_deps DESTINATION .
        DIRECTORIES ${DVA_DLL_DIR}
        PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
        POST_EXCLUDE_REGEXES ".*[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\].*")
      find_program(DVA_EGL_DLL NAMES libEGL.dll)
      if(DVA_EGL_DLL)
        install(FILES ${DVA_EGL_DLL} DESTINATION .)
      else()
        message(WARNING "libEGL.dll (ANGLE) not found: an installed copy will not be able to open its window")
      endif()
    else()
      install(TARGETS dejavuamp RUNTIME DESTINATION .)
    endif()
    install(DIRECTORY assets/ DESTINATION assets)
    install(FILES music/README.txt DESTINATION music)
    install(FILES eq-presets/README.txt DESTINATION eq-presets)
    install(FILES themes/README.txt DESTINATION themes)
    install(FILES README.md DESTINATION .)
    if(DVA_BUILD_TESTS)
      enable_testing()
      add_test(NAME version COMMAND dejavuamp --version)
      set_tests_properties(version PROPERTIES PASS_REGULAR_EXPRESSION "^dejavuamp [0-9]+[.][0-9]+")
      add_test(NAME window_smoke COMMAND dejavuamp --folder ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke --shot 90 ${CMAKE_CURRENT_BINARY_DIR}/smoke.ppm ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/tone.flac)
      # Linux: SDL's "offscreen" video driver (no screen needed). Windows: ANGLE has no offscreen device, so the test opens a real window for a second.
      if(WIN32)
        set(DVA_SMOKE_ENV "ASAN_OPTIONS=detect_leaks=0")
      else()
        set(DVA_SMOKE_ENV "SDL_VIDEODRIVER=offscreen;ASAN_OPTIONS=detect_leaks=0")
      endif()
      set_tests_properties(window_smoke PROPERTIES ENVIRONMENT "${DVA_SMOKE_ENV}")
    endif()
  else()
    message(STATUS "SDL2 or GLES2 development files not found: not building the dejavuamp window")
  endif()
endif()

if(DVA_BUILD_TESTS)
  enable_testing()
  # One program with the groups core, theme, fuzz, dsp and play; each group is its own ctest entry.
  add_executable(dva_tests tests/main.cpp tests/core_tests.cpp tests/theme_tests.cpp tests/fuzz_tests.cpp
                           tests/dsp_tests.cpp tests/play_tests.cpp)
  target_link_libraries(dva_tests PRIVATE dva_play)
  target_compile_definitions(dva_tests PRIVATE
    DVA_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/tests/parity/core.golden"
    DVA_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  foreach(group core theme fuzz dsp play)
    add_test(NAME ${group} COMMAND dva_tests ${group})
  endforeach()

  # Damaged songs and tag blocks for a set time (DVA_FUZZ_SECONDS, default 20). Run it built with -DDVA_SANITIZE=ON -DCMAKE_BUILD_TYPE=Release.
  add_executable(media_fuzz_test tests/media_fuzz_test.cpp)
  target_link_libraries(media_fuzz_test PRIVATE dva_play)
  target_compile_definitions(media_fuzz_test PRIVATE DVA_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  add_test(NAME media_fuzz COMMAND media_fuzz_test)
  set_tests_properties(media_fuzz PROPERTIES ENVIRONMENT "DVA_FUZZ_SECONDS=10;ASAN_OPTIONS=detect_leaks=0")

  find_program(DVA_NODE node)
  if(DVA_NODE)
    add_custom_target(parity-golden
      COMMAND ${DVA_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/parity/gen_core_golden.js
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Recording the JavaScript reference's answers in tests/parity/*.golden")
  endif()
endif()
