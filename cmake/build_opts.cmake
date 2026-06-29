set(unlog_compiler_name "${CMAKE_CXX_COMPILER_ID}")
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" unlog_system_processor)

set(UNLOG_ARM64 FALSE)
set(UNLOG_X86_64 FALSE)

if(unlog_system_processor STREQUAL "x86_64")
    set(CMAKE_TARGET_ARCHITECTURE "x86_64" CACHE STRING "target architecture" FORCE)
    set(UNLOG_X86_64 TRUE)
else()
    if(unlog_system_processor STREQUAL "aarch64")
        set(CMAKE_TARGET_ARCHITECTURE "arm64" CACHE STRING "target architecture" FORCE)
    elseif(unlog_system_processor MATCHES "^arm")
        set(CMAKE_TARGET_ARCHITECTURE "arm" CACHE STRING "target architecture" FORCE)
    else()
        message(FATAL_ERROR "unlog not supported on on this architecture -- what exactly are you using?")
    endif()
    set(UNLOG_ARM64 TRUE)
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C>:-fsigned-char>"
        "$<$<COMPILE_LANGUAGE:CXX>:-fsigned-char>")
endif()

set(UNLOG_DEBUG_BUILD FALSE)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(UNLOG_DEBUG_BUILD TRUE)
endif()

set(unlog_diagnostic_default OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(unlog_diagnostic_default ON)
endif()

option(UNLOG_BUILD_TESTS "Build unlog test suite" ${UNLOG_IS_TOPLEVEL_PROJECT})
option(UNLOG_WARNINGS_AS_ERRORS "treat all warnings as errors. turn off for development, on for release" ${UNLOG_DEBUG_BUILD})
option(UNLOG_USE_LIBCXX "build C++ targets with libc++ instead of libstdc++ when using clang" OFF)
option(UNLOG_DIAGNOSTIC "enable zifr diagnostic counters" ${unlog_diagnostic_default})

if(UNLOG_USE_LIBCXX)
    if(NOT unlog_compiler_name MATCHES "Clang")
        message(FATAL_ERROR "UNLOG_USE_LIBCXX requires clang; compiler is ${unlog_compiler_name}")
    endif()

    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>")
    add_link_options("$<$<LINK_LANGUAGE:CXX>:-stdlib=libc++>" "$<$<LINK_LANGUAGE:CXX>:-fuse-ld=lld>")
endif()

