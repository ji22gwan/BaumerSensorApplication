cmake_minimum_required(VERSION 3.16)
project(BaumerSensorApplication VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(BAUMER_BUILD_VIEWER "Build the GUI viewer (needs GLFW + OpenGL)" ON)
option(BAUMER_BUILD_TESTS  "Build tests and the UDP simulator" ON)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------------------------
# Baumer OX-Line SDK (vendored in third_party/OXApi, see its LICENSE AGREEMENT)
# ---------------------------------------------------------------------------------------------
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(OX_ARCH amd64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(OX_ARCH arm64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
  set(OX_ARCH armhf)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i.86")
  set(OX_ARCH i386)
else()
  message(FATAL_ERROR "No libOXApi.so for CPU '${CMAKE_SYSTEM_PROCESSOR}'")
endif()
set(OXAPI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/OXApi)
add_library(OXApi SHARED IMPORTED)
set_target_properties(OXApi PROPERTIES
  IMPORTED_LOCATION ${OXAPI_DIR}/lib/${OX_ARCH}/libOXApi.so
  IMPORTED_SONAME   libOXApi.so
  INTERFACE_INCLUDE_DIRECTORIES ${OXAPI_DIR}/include)

# ---------------------------------------------------------------------------------------------
# baumer_sensor: the library that is meant to be embedded into other controllers.
# The public headers do not expose any Baumer SDK header (pimpl).
# ---------------------------------------------------------------------------------------------
add_library(baumer_sensor STATIC lib/src/BaumerSensor.cpp)
add_library(Baumer::sensor ALIAS baumer_sensor)
target_include_directories(baumer_sensor PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/lib/include)
target_include_directories(baumer_sensor PRIVATE ${OXAPI_DIR}/include)
target_link_libraries(baumer_sensor PUBLIC $<LINK_ONLY:OXApi> Threads::Threads)
target_compile_options(baumer_sensor PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------------------------------------
# Viewer
# ---------------------------------------------------------------------------------------------
if(BAUMER_BUILD_VIEWER)
  find_package(glfw3 3.3 REQUIRED)
  find_package(OpenGL REQUIRED)

  set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
  set(IMPLOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/implot)
  add_library(imgui_implot STATIC
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMPLOT_DIR}/implot.cpp ${IMPLOT_DIR}/implot_items.cpp)
  target_include_directories(imgui_implot PUBLIC ${IMGUI_DIR} ${IMPLOT_DIR})
  target_link_libraries(imgui_implot PUBLIC glfw OpenGL::GL ${CMAKE_DL_LIBS})

  add_executable(BaumerSensorApplication app/main.cpp app/Viewer.cpp app/DemoSensor.cpp)
  target_link_libraries(BaumerSensorApplication PRIVATE baumer_sensor imgui_implot)
  target_compile_options(BaumerSensorApplication PRIVATE -Wall -Wextra)
  set_target_properties(BaumerSensorApplication PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
  install(TARGETS BaumerSensorApplication RUNTIME DESTINATION bin)
endif()

# ---------------------------------------------------------------------------------------------
# Simulator + tests
# ---------------------------------------------------------------------------------------------
if(BAUMER_BUILD_TESTS)
  enable_testing()
  add_executable(ox_udp_sim tools/ox_udp_sim.cpp)
  target_compile_options(ox_udp_sim PRIVATE -Wall -Wextra)
  add_executable(udp_loopback_test tests/udp_loopback_test.cpp)
  target_link_libraries(udp_loopback_test PRIVATE baumer_sensor)
  target_compile_options(udp_loopback_test PRIVATE -Wall -Wextra)
  add_test(NAME udp_loopback COMMAND udp_loopback_test)
  install(TARGETS ox_udp_sim RUNTIME DESTINATION bin)
endif()

install(FILES ${OXAPI_DIR}/lib/${OX_ARCH}/libOXApi.so DESTINATION lib)
