cmake_minimum_required(VERSION 4.0)
project(libpcache C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    # Conforming preprocessor (needed by tst.h's variadic macros), UTF-8 sources,
    # and no "use the _s variant" deprecation noise for standard C functions.
    add_compile_options(/W3 /Zc:preprocessor /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    # Put pcache.dll next to the test executables so they can load it.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

include(FetchContent)

FetchContent_Declare(
    xxHash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG        v0.8.3
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  cmake_unofficial
)
set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(xxHash)

if(WIN32)
    # No pkg-config on Windows: use CMake's FindSQLite3 (e.g. with the vcpkg toolchain).
    find_package(SQLite3 REQUIRED)
    set(SQLITE3_INCLUDE_DIRS ${SQLite3_INCLUDE_DIRS})
    set(SQLITE3_LIBRARIES SQLite::SQLite3)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SQLITE3 REQUIRED sqlite3)
endif()

include_directories(${SQLITE3_INCLUDE_DIRS})

add_library(pcache SHARED
    src/handle.c
    src/db.c
    src/volume.c
    src/pages.c
    src/pages_util.c
    src/maintenance.c
    src/platform.c
)
target_include_directories(pcache PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(pcache ${SQLITE3_LIBRARIES} xxHash::xxhash)
target_compile_definitions(pcache PRIVATE PCACHE_TESTING)

set_target_properties(pcache PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
    # The public header carries no dllexport annotations; export every symbol
    # (the tests also link against internal helpers).
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

install(TARGETS pcache
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES
    include/libpcache.h
    include/libpcache_errors.h
    include/module.modulemap
    DESTINATION include
)

enable_testing()
add_subdirectory(tests)
