cmake_minimum_required(VERSION 3.16)
project(doorlock_sniper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FAKE_ONLY "Build without Galaxy camera SDK (fake camera only)" OFF)

# Dependencies
find_package(OpenCV 4 REQUIRED COMPONENTS core imgproc highgui imgcodecs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0>=1.4 gstreamer-app-1.0>=1.4)
find_package(yaml-cpp REQUIRED)

# Single binary
add_executable(doorlock_sniper
    src/main.cpp
    src/camera.cpp
    src/pipeline.cpp
    src/serial.cpp
    src/display.cpp
    src/config.cpp
)

target_include_directories(doorlock_sniper PRIVATE
    src/
    ${GST_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_directories(doorlock_sniper PRIVATE
    ${GST_LIBRARY_DIRS}
)

target_link_libraries(doorlock_sniper PRIVATE
    ${OpenCV_LIBRARIES}
    ${GST_LIBRARIES}
    yaml-cpp
    dl
    pthread
)

if(FAKE_ONLY)
    # No Galaxy SDK needed — only fake camera mode works
    target_compile_definitions(doorlock_sniper PRIVATE DOORLOCK_FAKE_ONLY)
    message(STATUS "FAKE_ONLY build: Galaxy camera SDK disabled")
else()
    # Full build with Galaxy SDK
    set(GALAXY_SDK_DIR "${CMAKE_SOURCE_DIR}/../Galaxy_camera")
    set(GALAXY_INC "${GALAXY_SDK_DIR}/inc")
    set(GALAXY_LIB "${GALAXY_SDK_DIR}/lib/x86_64")

    target_include_directories(doorlock_sniper PRIVATE ${GALAXY_INC})
    target_link_directories(doorlock_sniper PRIVATE ${GALAXY_LIB})
    target_link_libraries(doorlock_sniper PRIVATE gxiapi)

    set_target_properties(doorlock_sniper PROPERTIES
        BUILD_RPATH "${GALAXY_LIB}"
        INSTALL_RPATH "$ORIGIN/../lib"
    )
    message(STATUS "Full build: Galaxy camera SDK at ${GALAXY_SDK_DIR}")
endif()

target_compile_options(doorlock_sniper PRIVATE -Wall -Wextra -O2)
