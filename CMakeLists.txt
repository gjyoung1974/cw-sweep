cmake_minimum_required(VERSION 3.20)
project(cwsweep VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(CWSWEEP_NATIVE "Optimise for the build machine's CPU" ON)
option(CWSWEEP_PULSE "Enable PulseAudio/PipeWire sound card input" ON)
option(CWSWEEP_SOAPY "Enable direct SoapySDR input" ON)

add_compile_options(-Wall -Wextra)
if(CWSWEEP_NATIVE AND CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-march=native)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)

# ---- core library: DSP, detection, decoding, spotting, synthesis ----------
add_library(cwcore STATIC
  src/core/morse.cpp
  src/core/lm.cpp
  src/core/stft.cpp
  src/core/keyer.cpp
  src/core/decoder.cpp
  src/core/softdecoder.cpp
  src/core/callsign.cpp
  src/core/engine.cpp
  src/core/synth.cpp
)
target_include_directories(cwcore PUBLIC src/core)
target_link_libraries(cwcore PUBLIC PkgConfig::FFTW3F Threads::Threads)

# ---- main CLI ----------------------------------------------------------------
add_executable(cwsweep
  app/main.cpp
  app/sources.cpp
  app/hpsdr.cpp
  app/rigctl.cpp
  app/telnet.cpp
)
target_link_libraries(cwsweep PRIVATE cwcore PkgConfig::SNDFILE)

if(CWSWEEP_PULSE)
  pkg_check_modules(PULSE IMPORTED_TARGET libpulse-simple)
  if(PULSE_FOUND)
    target_compile_definitions(cwsweep PRIVATE CWSWEEP_HAVE_PULSE)
    target_link_libraries(cwsweep PRIVATE PkgConfig::PULSE)
  endif()
endif()
if(CWSWEEP_SOAPY)
  pkg_check_modules(SOAPY IMPORTED_TARGET SoapySDR)
  if(SOAPY_FOUND)
    target_compile_definitions(cwsweep PRIVATE CWSWEEP_HAVE_SOAPY)
    target_link_libraries(cwsweep PRIVATE PkgConfig::SOAPY)
  endif()
endif()

# ---- tools -------------------------------------------------------------------
add_executable(cwgen tools/cwgen.cpp)
target_link_libraries(cwgen PRIVATE cwcore PkgConfig::SNDFILE)

add_executable(cwbench tools/cwbench.cpp)
target_link_libraries(cwbench PRIVATE cwcore)

# ---- tests -------------------------------------------------------------------
enable_testing()
add_executable(cwtests tests/tests.cpp)
target_link_libraries(cwtests PRIVATE cwcore)
add_test(NAME cwtests COMMAND cwtests)

# The hpsdr source against a stand-in radio on the loopback interface.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME hpsdr COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/hpsdr_test.py
           $<TARGET_FILE:cwsweep> $<TARGET_FILE:cwgen> ${CMAKE_BINARY_DIR}/hpsdr_test)
  # --rigctl against a stand-in rigctld; plays 45 s of audio in real time.
  add_test(NAME rigctl COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/rigctl_test.py
           $<TARGET_FILE:cwsweep> $<TARGET_FILE:cwgen> ${CMAKE_BINARY_DIR}/rigctl_test)
endif()

install(TARGETS cwsweep cwgen cwbench RUNTIME DESTINATION bin)
install(PROGRAMS contrib/cwsweep-forward DESTINATION bin)
install(DIRECTORY contrib/systemd DESTINATION share/cwsweep)
install(FILES docs/daemon.md DESTINATION share/cwsweep)
