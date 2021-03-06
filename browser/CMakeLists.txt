# bootstrap build system
cmake_minimum_required(VERSION 3.10 FATAL_ERROR)
project(vts-browser)
include(cmake/buildsys_initial_validation.cmake)
include(cmake/buildsys_xcode_codesign.cmake)

# by default, this is OSS build (can be overridden by customer machinery)
set(BUILDSYS_DEFAULT_CUSTOMER_NAME oss)
set(BUILDSYS_CPP_STANDARD 14)
include(buildsys/cmake/buildsys.cmake)
enable_hidden_visibility()

# bump shared libraries version here
set(vts-browser_SO_VERSION 0.0.0)

# include additional buildsys functions
include(cmake/buildsys_ide_groups.cmake)
include(cmake/buildsys_pack_data.cmake)

# find 3rd party dependencies
find_package(Boost 1.46 REQUIRED
    COMPONENTS thread program_options filesystem system
    date_time serialization regex chrono iostreams)
link_directories(${Boost_LIBRARY_DIRS})
include_directories(SYSTEM ${Boost_INCLUDE_DIRS})
find_package(Eigen3 REQUIRED)
include_directories(SYSTEM ${EIGEN3_INCLUDE_DIR})
add_subdirectory(externals/utfcpp)

# Optick - instrumental profiler for games
find_package(Optick QUIET)
if(NOT TARGET Optick)
    add_subdirectory(src/dummy-optick)
endif()

# libraries type
set(VTS_BROWSER_TYPE FRAMEWORK CACHE STRING "Type of browser libraries")
set_property(CACHE VTS_BROWSER_TYPE PROPERTY STRINGS STATIC SHARED MODULE FRAMEWORK)
set(VTS_BROWSER_BUILD_LIBRARY ${VTS_BROWSER_TYPE})
if(VTS_BROWSER_TYPE STREQUAL "STATIC")
    set(VTS_BROWSER_BUILD_VISIBILITY PUBLIC)
    set(VTS_BROWSER_BUILD_MACRO STATIC)
else()
    set(VTS_BROWSER_BUILD_VISIBILITY PRIVATE)
    set(VTS_BROWSER_BUILD_MACRO SHARED)
endif()
if(VTS_BROWSER_TYPE STREQUAL "FRAMEWORK")
    set(VTS_BROWSER_BUILD_LIBRARY SHARED)
endif()

if(UNIX)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-parameter") # this drives me crazy! stop it
endif()

# vts browser library
message(STATUS "including vts browser library")
add_subdirectory(src/vts-libbrowser)
include_directories(src/vts-libbrowser/include)

if(VTS_BROWSER_TYPE STREQUAL "MODULE")
    return()
endif()

# vts renderer library
message(STATUS "including vts renderer library")
add_subdirectory(externals/glad)
add_subdirectory(src/vts-librenderer)
include_directories(src/vts-librenderer/include)

# application
if(BUILDSYS_WASM)
    message(STATUS "including vts-browser-wasm")
    add_subdirectory(src/vts-browser-wasm-canvas)
    add_subdirectory(src/vts-browser-wasm-sdl)
elseif(BUILDSYS_UWP)
    message(STATUS "including vts-browser-uwp")
    add_subdirectory(src/vts-browser-uwp)
elseif(BUILDSYS_IOS)
    message(STATUS "including vts-browser-ios")
    add_subdirectory(src/vts-browser-ios)
else()
    # desktop apps (GLFW)
    find_package(glfw3 QUIET)
    if(TARGET glfw)
        # desktop application
        message(STATUS "including vts-browser-desktop")
        add_subdirectory(src/vts-browser-desktop)
    else()
        message(WARNING "GLFW was not found, the desktop application is skipped")
    endif()

    # desktop apps (SDL)
    cmake_policy(SET CMP0004 OLD) # because SDL installed on some systems has improperly configured libraries
    find_package(SDL2 QUIET)
    if((TARGET SDL2) OR (DEFINED SDL2_LIBRARIES))
        include_directories(SYSTEM ${SDL2_INCLUDE_DIR})

        # minimal applications
        message(STATUS "including vts-browser-minimal")
        add_subdirectory(src/vts-browser-minimal-c)
        add_subdirectory(src/vts-browser-minimal-cpp)

        # two cameras
        message(STATUS "including two-cameras")
        add_subdirectory(src/vts-browser-two-cameras)
    else()
        message(WARNING "SDL was not found, some example applications are skipped")
    endif()

    # desktop apps (Qt)
    find_package(Qt5 COMPONENTS Core Gui QUIET)
    if(TARGET Qt5::Gui)
        # Qt application
        message(STATUS "including vts-browser-qt")
        add_subdirectory(src/vts-browser-qt)
    else()
        message(WARNING "Qt5 was not found, the example application is skipped")
    endif()
endif()

# vts csharp libraries
if(MSVC)
    message(STATUS "including csharp libraries")
    set(CMAKE_CSharp_FLAGS "${CMAKE_CSharp_FLAGS} -langversion:6 -unsafe")
    add_subdirectory(src/vts-libbrowser-cs)
    add_subdirectory(src/vts-librenderer-cs)
    add_subdirectory(src/vts-browser-minimal-cs)
endif()
