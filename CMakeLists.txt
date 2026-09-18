cmake_minimum_required(VERSION 3.14)

project(event-hub-cpp
    VERSION 0.1.0
    DESCRIPTION "Lightweight C++ event bus for modular applications"
    LANGUAGES CXX
)

include(GNUInstallDirs)
find_package(Threads REQUIRED)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(EVENT_HUB_CPP_TOP_LEVEL ON)
else()
    set(EVENT_HUB_CPP_TOP_LEVEL OFF)
endif()

option(EVENT_HUB_CPP_BUILD_EXAMPLES "Build event-hub-cpp examples" ${EVENT_HUB_CPP_TOP_LEVEL})
option(EVENT_HUB_CPP_BUILD_TESTS "Build event-hub-cpp tests" ${EVENT_HUB_CPP_TOP_LEVEL})
option(EVENT_HUB_CPP_USE_TIME_SHIELD "Enable optional time-shield-cpp integration" OFF)
option(EVENT_HUB_CPP_USE_TIME_SHIELD_NTP "Enable time-shield-cpp NTP support" OFF)
set(EVENT_HUB_CPP_TIME_SHIELD_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/external/time-shield-cpp
    CACHE PATH "Path to the optional time-shield-cpp submodule"
)
set(EVENT_HUB_CPP_TEST_TIMEOUT_SECONDS
    15
    CACHE STRING "Per-test timeout for event-hub-cpp CTest tests"
)

add_library(event_hub_cpp INTERFACE)
add_library(event_hub::event_hub ALIAS event_hub_cpp)
add_library(event-hub-cpp::event-hub-cpp ALIAS event_hub_cpp)

set_target_properties(event_hub_cpp PROPERTIES EXPORT_NAME event_hub)

target_compile_features(event_hub_cpp INTERFACE cxx_std_17)
target_link_libraries(event_hub_cpp INTERFACE Threads::Threads)
target_include_directories(event_hub_cpp
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(EVENT_HUB_CPP_USE_TIME_SHIELD)
    if(CMAKE_VERSION VERSION_LESS 3.15)
        message(FATAL_ERROR
            "EVENT_HUB_CPP_USE_TIME_SHIELD requires CMake 3.15 or newer "
            "because external/time-shield-cpp requires it."
        )
    endif()

    if(NOT EXISTS "${EVENT_HUB_CPP_TIME_SHIELD_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "EVENT_HUB_CPP_USE_TIME_SHIELD=ON requires the "
            "external/time-shield-cpp submodule. Run: "
            "git submodule update --init --recursive external/time-shield-cpp"
        )
    endif()

    set(TIME_SHIELD_ENABLE_NTP_CLIENT
        ${EVENT_HUB_CPP_USE_TIME_SHIELD_NTP}
        CACHE BOOL "Enable time-shield-cpp NTP client" FORCE
    )
    set(TIME_SHIELD_CPP_BUILD_EXAMPLES
        OFF
        CACHE BOOL "Build Time Shield examples" FORCE
    )
    set(TIME_SHIELD_CPP_BUILD_TESTS
        OFF
        CACHE BOOL "Build Time Shield tests" FORCE
    )

    add_subdirectory(
        ${EVENT_HUB_CPP_TIME_SHIELD_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/external/time-shield-cpp
    )
    target_link_libraries(event_hub_cpp INTERFACE time_shield::time_shield)
    target_compile_definitions(event_hub_cpp
        INTERFACE
            EVENT_HUB_CPP_USE_TIME_SHIELD=1
    )

    if(EVENT_HUB_CPP_USE_TIME_SHIELD_NTP)
        target_compile_definitions(event_hub_cpp
            INTERFACE EVENT_HUB_CPP_USE_TIME_SHIELD_NTP=1)
    else()
        target_compile_definitions(event_hub_cpp
            INTERFACE EVENT_HUB_CPP_USE_TIME_SHIELD_NTP=0)
    endif()
else()
    target_compile_definitions(event_hub_cpp
        INTERFACE
            EVENT_HUB_CPP_USE_TIME_SHIELD=0
            EVENT_HUB_CPP_USE_TIME_SHIELD_NTP=0
    )
endif()

set(EVENT_HUB_CPP_CONFIG_USES_TIME_SHIELD ${EVENT_HUB_CPP_USE_TIME_SHIELD})

# pkg-config files live below the configured libdir. Compute the relative
# prefix rather than assuming a single-component libdir (e.g. lib/x86_64-linux-gnu).
string(REPLACE "/" ";" EVENT_HUB_CPP_LIBDIR_COMPONENTS "${CMAKE_INSTALL_LIBDIR}")
list(LENGTH EVENT_HUB_CPP_LIBDIR_COMPONENTS EVENT_HUB_CPP_LIBDIR_DEPTH)
math(EXPR EVENT_HUB_CPP_PC_PREFIX_HOPS "${EVENT_HUB_CPP_LIBDIR_DEPTH} + 1")
set(EVENT_HUB_CPP_PC_PREFIX_REL "")
foreach(_hop RANGE 1 ${EVENT_HUB_CPP_PC_PREFIX_HOPS})
    set(EVENT_HUB_CPP_PC_PREFIX_REL "../${EVENT_HUB_CPP_PC_PREFIX_REL}")
endforeach()

if(EVENT_HUB_CPP_USE_TIME_SHIELD)
    if(EVENT_HUB_CPP_USE_TIME_SHIELD_NTP)
        set(EVENT_HUB_CPP_PC_TIME_SHIELD_NTP 1)
    else()
        set(EVENT_HUB_CPP_PC_TIME_SHIELD_NTP 0)
    endif()
    # Keep the pkg-config metadata usable with the currently checked-out
    # submodule; the dependency floor is raised to 2.0.0 with the v2 update.
    set(EVENT_HUB_CPP_PC_TIME_SHIELD_REQUIRES "time-shield")
    set(EVENT_HUB_CPP_PC_TIME_SHIELD_CFLAGS
        "-DEVENT_HUB_CPP_USE_TIME_SHIELD=1 -DEVENT_HUB_CPP_USE_TIME_SHIELD_NTP=${EVENT_HUB_CPP_PC_TIME_SHIELD_NTP}")
else()
    set(EVENT_HUB_CPP_PC_TIME_SHIELD_REQUIRES "")
    set(EVENT_HUB_CPP_PC_TIME_SHIELD_CFLAGS
        "-DEVENT_HUB_CPP_USE_TIME_SHIELD=0 -DEVENT_HUB_CPP_USE_TIME_SHIELD_NTP=0")
endif()

if(MSVC)
    set(EVENT_HUB_CPP_WARNING_FLAGS /W4)
else()
    set(EVENT_HUB_CPP_WARNING_FLAGS -Wall -Wextra -Wpedantic)
endif()

file(GLOB_RECURSE EVENT_HUB_CPP_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp
)
add_custom_target(event_hub_cpp_headers SOURCES ${EVENT_HUB_CPP_HEADERS})
set_source_files_properties(${EVENT_HUB_CPP_HEADERS} PROPERTIES HEADER_FILE_ONLY ON)

function(event_hub_cpp_add_executable target_name source_path)
    add_executable(${target_name} ${source_path})
    target_link_libraries(${target_name} PRIVATE event_hub::event_hub)
    if(EVENT_HUB_CPP_WARNING_FLAGS)
        target_compile_options(${target_name} PRIVATE ${EVENT_HUB_CPP_WARNING_FLAGS})
    endif()
    target_sources(${target_name} PRIVATE ${EVENT_HUB_CPP_HEADERS})
endfunction()

if(EVENT_HUB_CPP_BUILD_EXAMPLES)
    file(GLOB EVENT_HUB_CPP_EXAMPLE_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp
    )
    foreach(example_source IN LISTS EVENT_HUB_CPP_EXAMPLE_SOURCES)
        get_filename_component(example_name ${example_source} NAME_WE)
        event_hub_cpp_add_executable(event_hub_${example_name} ${example_source})
    endforeach()
endif()

if(EVENT_HUB_CPP_BUILD_TESTS)
    enable_testing()

    file(GLOB EVENT_HUB_CPP_TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp
    )
    foreach(test_source IN LISTS EVENT_HUB_CPP_TEST_SOURCES)
        get_filename_component(test_name ${test_source} NAME_WE)
        if(test_name STREQUAL "test_calendar_scheduler" AND NOT EVENT_HUB_CPP_USE_TIME_SHIELD)
            continue()
        endif()

        set(test_target event_hub_${test_name})
        event_hub_cpp_add_executable(${test_target} ${test_source})
        add_test(NAME ${test_target} COMMAND ${test_target})
        set_tests_properties(${test_target}
            PROPERTIES TIMEOUT ${EVENT_HUB_CPP_TEST_TIMEOUT_SECONDS}
        )
    endforeach()
endif()

include(CMakePackageConfigHelpers)

set(EVENT_HUB_CPP_INSTALL_CMAKEDIR
    ${CMAKE_INSTALL_LIBDIR}/cmake/event-hub-cpp
    CACHE STRING "CMake package config install directory"
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/event-hub-cppConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/event-hub-cppConfig.cmake
    INSTALL_DESTINATION ${EVENT_HUB_CPP_INSTALL_CMAKEDIR}
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/event-hub-cppConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/event-hub-cpp.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/event-hub-cpp.pc
    @ONLY
)

install(TARGETS event_hub_cpp EXPORT event-hub-cppTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT event-hub-cppTargets
    NAMESPACE event_hub::
    FILE event-hub-cppTargets.cmake
    DESTINATION ${EVENT_HUB_CPP_INSTALL_CMAKEDIR}
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/event-hub-cppConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/event-hub-cppConfigVersion.cmake
    DESTINATION ${EVENT_HUB_CPP_INSTALL_CMAKEDIR}
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/event-hub-cpp.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)
