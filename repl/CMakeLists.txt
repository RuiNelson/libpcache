cmake_minimum_required(VERSION 4.0)
project(pcache_repl CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(FetchContent)

FetchContent_Declare(
    args
    GIT_REPOSITORY https://github.com/Taywee/args.git
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(args)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

add_executable(pcache_repl
    src/main.cxx
    src/commands.cxx
    src/errors.cxx
    src/lexer.cxx
    src/file_utils.cxx
    src/cmd_utils.cxx
    src/runner.cxx
    src/cmd_help.cxx
    src/cmd_close.cxx
    src/cmd_put.cxx
    src/cmd_get.cxx
    src/cmd_check.cxx
    src/cmd_delete.cxx
    src/cmd_pages.cxx
    src/cmd_inspect.cxx
    src/cmd_set_max_pages.cxx
    src/cmd_defragment.cxx
)
target_include_directories(pcache_repl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${args_SOURCE_DIR}/src
)
target_link_directories(pcache_repl PRIVATE
    ${SQLITE3_LIBRARY_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../build
)
target_link_libraries(pcache_repl
    ${SQLITE3_LIBRARIES}
    taywee::args
    readline
    pcache
)