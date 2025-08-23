include(ExternalProject)

find_package(Git)
if(GIT_FOUND)
    message(STATUS "Checking submodules")
    include(check_submodule)
    check_submodule(fmt)
    check_submodule(spdlog)
endif()

# fmt
add_subdirectory(fmt)

# spdlog
set(SPDLOG_BUILD_PIC ON CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL_HO ON CACHE BOOL "" FORCE)
add_subdirectory(spdlog)
