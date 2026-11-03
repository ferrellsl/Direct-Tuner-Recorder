cmake_minimum_required(VERSION 3.16)
project(DirectTunerRecorder LANGUAGES CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# src/core is shared with the Kodi add-on (github.com/ferrellsl/Direct-Tuner-PVR) and has no Kodi code.
file(GLOB CORE_SOURCES src/core/*.cpp)
add_executable(DirectTunerRecorder
  ${CORE_SOURCES}
  src/service/Fetch.cpp
  src/service/HttpServer.cpp
  src/service/Main.cpp
  src/service/Recorder.cpp)

if(WIN32)
  target_compile_definitions(DirectTunerRecorder PRIVATE
    TARGET_WINDOWS NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
  target_compile_options(DirectTunerRecorder PRIVATE /utf-8 /W3)
  target_link_libraries(DirectTunerRecorder PRIVATE ws2_32 iphlpapi winhttp advapi32)
  # One file to install: the C++ runtime goes inside the program.
  set_target_properties(DirectTunerRecorder PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
  find_package(Threads REQUIRED)
  target_compile_definitions(DirectTunerRecorder PRIVATE _FILE_OFFSET_BITS=64)
  target_compile_options(DirectTunerRecorder PRIVATE -Wall)
  target_link_libraries(DirectTunerRecorder PRIVATE Threads::Threads)
endif()

install(TARGETS DirectTunerRecorder RUNTIME DESTINATION bin)
