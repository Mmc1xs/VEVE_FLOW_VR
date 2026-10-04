cmake_minimum_required(VERSION 3.15)

project(flow_dashboard_helper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(OPENVR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../openvr" ABSOLUTE)

get_filename_component(DRIVER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../flow_steamvr_driver/src" ABSOLUTE)

add_executable(flow_dashboard_helper
  main.cpp
  desktop_layer_streamer.h
  desktop_layer_streamer.cpp
  "${DRIVER_SRC}/flow_nvenc_encoder.cpp"  # shared with the driver
)
target_include_directories(flow_dashboard_helper PRIVATE
  "${OPENVR_ROOT}/headers"
  "${CMAKE_CURRENT_SOURCE_DIR}/compat"
  "${DRIVER_SRC}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nv-codec-headers/include"
)
target_link_libraries(flow_dashboard_helper PRIVATE "${OPENVR_ROOT}/lib/win64/openvr_api.lib" ws2_32 d3d11 dxgi)

# No console window: SteamVR launches it in the background. main() stays the entry point.
set_target_properties(flow_dashboard_helper PROPERTIES
  WIN32_EXECUTABLE TRUE
  LINK_FLAGS "/ENTRY:mainCRTStartup"
  RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/dist>"
)

# dist/ is what SteamVR registers: exe + openvr_api.dll + manifest side by side.
add_custom_command(TARGET flow_dashboard_helper POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OPENVR_ROOT}/bin/win64/openvr_api.dll" "${CMAKE_BINARY_DIR}/dist"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/flow_dashboard_helper.vrmanifest" "${CMAKE_BINARY_DIR}/dist"
)
