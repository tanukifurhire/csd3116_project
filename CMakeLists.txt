# CMakeLists.txt -- the drifto *client* for Windows (MSVC + vcpkg).
#
# Linux keeps using the makefiles (`make drifto`, servers, tests); this file
# only builds drifto.exe (client_folder + game + gfx + net [+ audio]).
# Dependencies come from vcpkg.json (manifest mode): glfw3, cyclonedds
# (security + ssl + idlc), openssl. See README.md "Windows" and
# tools/windows/build.bat / tools/windows/sync_and_build.sh.
#
#   cmake -S . -B build/windows -G "Visual Studio 17 2022" -A x64 ^
#         -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
#   cmake --build build/windows --config Release
#
# Modules are globbed (<module>/src/*.cpp, *.c), so a new module only needs a
# line in DRIFTO_MODULES plus its dependencies in drifto_module_deps() below.
cmake_minimum_required(VERSION 3.21)
project(drifto LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11) # idlc output uses _Alignof
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(glfw3 CONFIG REQUIRED)
find_package(CycloneDDS CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(OpenGL REQUIRED)

# ---------------------------------------------------------------- compiler flags
add_library(drifto_flags INTERFACE)
target_include_directories(drifto_flags INTERFACE
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/third_party
  ${CMAKE_SOURCE_DIR}/third_party/khronos) # GL/glcorearb.h, KHR/khrplatform.h
if(MSVC)
  target_compile_options(drifto_flags INTERFACE /W3 /permissive- /utf-8 /Zc:__cplusplus /MP)
  target_compile_definitions(drifto_flags INTERFACE
    NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
else()
  target_compile_options(drifto_flags INTERFACE -Wall -Wextra)
endif()

# ---------------------------------------------------------------- IDL (net)
set(DRIFTO_IDL ${CMAKE_SOURCE_DIR}/net/idl/drifto_net.idl)
set(DRIFTO_GEN_DIR ${CMAKE_BINARY_DIR}/net/gen)
find_program(DRIFTO_IDLC NAMES idlc
  HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/cyclonedds"
        "${CycloneDDS_DIR}/../../tools/cyclonedds")
# Fallback: C generated elsewhere (e.g. copied from Linux build/net/gen)
set(DRIFTO_IDL_PREGEN "" CACHE PATH "Directory with pre-generated drifto_net.c/.h (used when idlc is not found)")
file(MAKE_DIRECTORY ${DRIFTO_GEN_DIR})
if(DRIFTO_IDLC)
  message(STATUS "idlc: ${DRIFTO_IDLC}")
  add_custom_command(
    OUTPUT ${DRIFTO_GEN_DIR}/drifto_net.c ${DRIFTO_GEN_DIR}/drifto_net.h
    COMMAND ${DRIFTO_IDLC} -l c -x final -o ${DRIFTO_GEN_DIR} ${DRIFTO_IDL}
    DEPENDS ${DRIFTO_IDL}
    COMMENT "idlc drifto_net.idl")
elseif(DRIFTO_IDL_PREGEN)
  message(STATUS "idlc not found, using pre-generated IDL code from ${DRIFTO_IDL_PREGEN}")
  configure_file(${DRIFTO_IDL_PREGEN}/drifto_net.c ${DRIFTO_GEN_DIR}/drifto_net.c COPYONLY)
  configure_file(${DRIFTO_IDL_PREGEN}/drifto_net.h ${DRIFTO_GEN_DIR}/drifto_net.h COPYONLY)
else()
  message(FATAL_ERROR "idlc not found (vcpkg cyclonedds[idlc]) and DRIFTO_IDL_PREGEN not set")
endif()

# ---------------------------------------------------------------- modules
set(DRIFTO_MODULES game gfx net audio)

function(drifto_module_deps mod)
  if(mod STREQUAL "gfx")
    target_link_libraries(drifto_gfx PUBLIC glfw OpenGL::GL)
  elseif(mod STREQUAL "net")
    target_sources(drifto_net PRIVATE ${DRIFTO_GEN_DIR}/drifto_net.c)
    if(MSVC) # idlc output: enum-mixing in its opcode tables (harmless)
      set_source_files_properties(${DRIFTO_GEN_DIR}/drifto_net.c PROPERTIES COMPILE_OPTIONS "/wd5287")
    endif()
    target_include_directories(drifto_net PUBLIC ${DRIFTO_GEN_DIR})
    target_link_libraries(drifto_net PUBLIC CycloneDDS::ddsc OpenSSL::Crypto)
    if(WIN32)
      target_link_libraries(drifto_net PUBLIC ws2_32 iphlpapi)
    endif()
  elseif(mod STREQUAL "audio")
    # miniaudio + stb_vorbis are compiled in audio/src/miniaudio_impl.cpp
    # (third_party/ is on the include path); WASAPI is loaded at run time.
  endif()
endfunction()

set(DRIFTO_MODULE_TARGETS "")
foreach(mod IN LISTS DRIFTO_MODULES)
  file(GLOB _srcs CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/${mod}/src/*.cpp ${CMAKE_SOURCE_DIR}/${mod}/src/*.c)
  if(NOT _srcs AND NOT mod STREQUAL "net")
    continue()
  endif()
  add_library(drifto_${mod} STATIC ${_srcs})
  target_link_libraries(drifto_${mod} PUBLIC drifto_flags)
  drifto_module_deps(${mod})
  list(APPEND DRIFTO_MODULE_TARGETS drifto_${mod})
endforeach()

# ---------------------------------------------------------------- client exe
file(GLOB DRIFTO_CLIENT_SRCS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/client_folder/src/*.cpp)
add_executable(drifto ${DRIFTO_CLIENT_SRCS})
target_link_libraries(drifto PRIVATE drifto_flags ${DRIFTO_MODULE_TARGETS} glfw OpenGL::GL)
set_target_properties(drifto PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:drifto>")

# Headless LAN probe (net/tests/lan_probe.cpp): cross-OS LAN checks, no window.
add_executable(lan_probe ${CMAKE_SOURCE_DIR}/net/tests/lan_probe.cpp)
target_link_libraries(lan_probe PRIVATE drifto_flags drifto_net)

# ---------------------------------------------------------------- runtime files
# Everything drifto.exe needs at run time goes next to it, so it runs from
# any working directory (and by double-click).
set(_out "$<TARGET_FILE_DIR:drifto>")
foreach(dir gfx/shaders net/config certs assets)
  if(EXISTS ${CMAKE_SOURCE_DIR}/${dir})
    add_custom_command(TARGET drifto POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_SOURCE_DIR}/${dir} ${_out}/${dir})
  else()
    message(WARNING "${dir}/ not found -- drifto.exe will run without it (Linux: make certs / make assets, then sync)")
  endif()
endforeach()
# DDS Security plugins are loaded at run time (not linked), and need OpenSSL.
if(WIN32 AND DEFINED VCPKG_INSTALLED_DIR)
  set(_vbin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
  file(GLOB DRIFTO_RUNTIME_DLLS "${_vbin}/dds_security_*.dll" "${_vbin}/libssl*.dll" "${_vbin}/libcrypto*.dll")
  if(DRIFTO_RUNTIME_DLLS)
    add_custom_command(TARGET drifto POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DRIFTO_RUNTIME_DLLS} ${_out})
  else()
    message(WARNING "no dds_security_*.dll in ${_vbin}: DDS Security won't work (use --insecure)")
  endif()
endif()
