cmake_minimum_required(VERSION 3.15)

project(flowvr_steamvr_driver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT PLATFORM)
  if(CMAKE_SIZEOF_VOID_P MATCHES 8)
    set(PLATFORM 64)
  else()
    set(PLATFORM 32)
  endif()
endif()

if(WIN32)
  add_definitions(-D_WIN32)
  set(ARCH_TARGET win${PLATFORM})
else()
  message(FATAL_ERROR "This first Flow SteamVR driver scaffold is Windows-only.")
endif()

get_filename_component(OPENVR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../openvr" ABSOLUTE)
set(OPENVR_INCLUDE_DIR "${OPENVR_ROOT}/headers")

find_library(OPENVR_LIBRARIES
  NAMES openvr_api
  PATHS "${OPENVR_ROOT}/lib"
  PATH_SUFFIXES "win${PLATFORM}"
  NO_DEFAULT_PATH
)

if(NOT OPENVR_LIBRARIES)
  message(FATAL_ERROR "openvr_api library not found under ${OPENVR_ROOT}/lib/win${PLATFORM}")
endif()

add_subdirectory(utils/driverlog)
add_subdirectory(utils/vrmath)

set(TARGET_NAME flowvr)
set(DRIVER_NAME "driver_${TARGET_NAME}")

add_library(${DRIVER_NAME} SHARED
  src/hmd_driver_factory.cpp
  src/device_provider.h
  src/device_provider.cpp
  src/hmd_device_driver.h
  src/hmd_device_driver.cpp
  src/keyboard_mouse_controller.h
  src/keyboard_mouse_controller.cpp
  src/hand_controller.h
  src/hand_controller.cpp
  src/flow_shared_input.h
  src/flow_nvenc_encoder.h
  src/flow_nvenc_encoder.cpp
  src/virtual_display_device.h
  src/virtual_display_device.cpp
  src/flow_pose_sync.h
  src/flow_pointer_gate.h
  src/flow_audio_streamer.h
  src/flow_audio_streamer.cpp
)

target_include_directories(${DRIVER_NAME} PRIVATE
  "${OPENVR_INCLUDE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nv-codec-headers/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/utils/driverlog"
  "${CMAKE_CURRENT_SOURCE_DIR}/utils/vrmath"
)

target_link_libraries(${DRIVER_NAME} PRIVATE
  ${OPENVR_LIBRARIES}
  util_driverlog
  util_vrmath
  ws2_32
  iphlpapi
  d3d11
  dxgi
  d3dcompiler
  ole32
)

set_target_properties(${DRIVER_NAME} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/dist/${TARGET_NAME}/bin/${ARCH_TARGET}>"
)

add_custom_command(
  TARGET ${DRIVER_NAME}
  PRE_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/${TARGET_NAME}"
          "${CMAKE_BINARY_DIR}/dist/${TARGET_NAME}"
)
