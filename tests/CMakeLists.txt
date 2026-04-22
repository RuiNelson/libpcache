include(FetchContent)

FetchContent_Declare(
    tst
    URL https://raw.githubusercontent.com/rdentato/tst/main/src/tst.h
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(tst)

include_directories(${CMAKE_SOURCE_DIR})
include_directories(${tst_SOURCE_DIR})

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-variadic-macro-arguments -Wno-strict-prototypes)
endif()

add_executable(pcache_test test_pcache.c)
target_link_libraries(pcache_test pcache ${SQLITE3_LIBRARIES})
add_test(NAME pcache_test COMMAND pcache_test)

add_executable(benchmark_pcache benchmark_pcache.c)
target_link_libraries(benchmark_pcache pcache)
add_test(NAME benchmark_pcache COMMAND benchmark_pcache)
