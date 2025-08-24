cmake_minimum_required(VERSION 3.13...3.25)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(LANGS C CXX)
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
	foreach(lang ${LANGS})
    	if(NOT DEFINED CMAKE_${lang}_COMPILER_LAUNCHER AND NOT CMAKE_${lang}_COMPILER MATCHES ".*/ccache")
      		message(STATUS "Enabling ccache for ${lang}")
      		set(CMAKE_${lang}_COMPILER_LAUNCHER ${CCACHE_PROGRAM} CACHE STRING "")
    	endif()
  endforeach()
endif()

project(unlog
    VERSION 0.0.1
    DESCRIPTION "unnecessarily fast C++23 logging"
    LANGUAGES ${LANGS})

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(UNLOG_IS_TOPLEVEL_PROJECT TRUE)
else()
    set(UNLOG_IS_TOPLEVEL_PROJECT FALSE)
endif()

# TODO: not configured yet for shared vs static
option(BUILD_SHARED_LIBS "Build as shared library" OFF)
option(BUILD_STATIC_DEPS "Build and link against static dependencies" OFF)
option(WARNINGS_AS_ERRORS "Treat all warnings as errors. turn off for development, on for release" OFF)
option(UNLOG_BUILD_TESTS "Build unlog test suite" ${UNLOG_IS_TOPLEVEL_PROJECT})

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(default_lto ON)
if(WIN32 OR APPLE OR BUILD_STATIC_DEPS)
    set(default_lto OFF)
endif()
option(WITH_LTO "Compile with Link-time Optimization" ${default_lto})
if(WITH_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT IPO_ENABLED OUTPUT ipo_error)
    if(IPO_ENABLED)
        message(STATUS "LTO enabled")
    else()
        message(WARNING "LTO not supported by compiler: ${ipo_error}")
    endif()
else()
    message(STATUS "LTO disabled")
    set(IPO_ENABLED OFF)
endif()

set(IPO_ENABLED OFF)
if(IPO_ENABLED AND NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

add_library(unlog
    src/log.cpp
    src/logger.cpp
    src/utils.cpp
)

target_include_directories(unlog PUBLIC include)
target_compile_features(unlog PUBLIC cxx_std_23)

add_subdirectory(external)

target_link_libraries(unlog 
    PUBLIC 
    fmt::fmt
    spdlog::spdlog_header_only
)

set(warning_flags -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-function -Werror=vla -Wno-deprecated-declaration)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    list(APPEND warning_flags -Wno-unknown-warning-option)
endif()

if(WARNINGS_AS_ERRORS)
    list(APPEND warning_flags -Werror)
endif()

add_library(unlog_warnings INTERFACE)

target_compile_options(unlog_warnings INTERFACE "$<$<OR:$<COMPILE_LANGUAGE:CXX>,$<COMPILE_LANGUAGE:C>>:${warning_flags}>")

target_link_libraries(unlog INTERFACE unlog_warnings)

if(UNLOG_BUILD_TESTS)
    add_subdirectory(tests)
endif()

add_library(un::log ALIAS unlog)

# TODO: add install
# include(GNUInstallDirs)
