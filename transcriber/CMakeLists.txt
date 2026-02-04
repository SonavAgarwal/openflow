cmake_minimum_required(VERSION 3.20)
project(voicecli CXX)

# Apple-silicon only, Monterey+ so M1 works
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0)

# Force static libs so the binaries don't depend on libwhisper.dylib.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# Turn on the fast backends
set(GGML_METAL ON CACHE BOOL "" FORCE)
set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)   # embed default.metallib in the binary
# Core ML is disabled.
option(WHISPER_COREML "whisper: enable Core ML framework" OFF)

# We’ll control examples from here; you don’t need them
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)

# Bring in whisper.cpp submodule
add_subdirectory(whisper.cpp EXCLUDE_FROM_ALL)

# SDL2 for mic capture (you’re using common-sdl.*)
find_package(SDL2 REQUIRED)

# Your transcription binary
add_executable(transcriber
  transcription/stream.cpp
  transcription/common-sdl.cpp
)

target_include_directories(transcriber PRIVATE
  whisper.cpp                     # for include/whisper.h
  ${SDL2_INCLUDE_DIRS}
  transcription
)

target_link_libraries(transcriber PRIVATE
  whisper
  ${SDL2_LIBRARIES}
)

# Helpful rpath on macOS if you ever add frameworks/dylibs later
set_target_properties(transcriber PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(vad_transcriber
  transcription/vad.cpp
  transcription/common-sdl.cpp
)

target_include_directories(vad_transcriber PRIVATE
  whisper.cpp
  ${SDL2_INCLUDE_DIRS}
  transcription
)

target_link_libraries(vad_transcriber PRIVATE
  whisper
  ${SDL2_LIBRARIES}
)

set_target_properties(vad_transcriber PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
