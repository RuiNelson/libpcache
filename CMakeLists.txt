cmake_minimum_required(VERSION 4.3.1)
project(libpcache C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(FetchContent)

FetchContent_Declare(
    xxHash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG        v0.8.2
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  cmake_unofficial
)
FetchContent_MakeAvailable(xxHash)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

include_directories(${SQLITE3_INCLUDE_DIRS})

add_library(pcache SHARED
    src/handle.c
    src/db.c
    src/volume.c
    src/pages.c
    src/maintenance.c
)
target_include_directories(pcache PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(pcache ${SQLITE3_LIBRARIES} xxHash::xxhash)

set_target_properties(pcache PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

install(TARGETS pcache
    LIBRARY DESTINATION lib
)

install(FILES
    include/libpcache.h
    include/libpcache_errors.h
    include/module.modulemap
    DESTINATION include
)

enable_testing()
add_subdirectory(tests)
