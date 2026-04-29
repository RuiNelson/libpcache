include(FetchContent)

FetchContent_Declare(
    tst
    URL https://raw.githubusercontent.com/rdentato/tst/main/src/tst.h
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(tst)

include_directories(${CMAKE_SOURCE_DIR})
include_directories(${tst_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_compile_options(
        -Wno-variadic-macro-arguments-omitted
        -Wno-strict-prototypes
        -Wno-extra-semi
        -Wno-unused-parameter
        -Wno-unused-function
    )
endif()

add_library(pcache_test_common STATIC common.c)
target_include_directories(pcache_test_common PUBLIC ${CMAKE_SOURCE_DIR}/include)

set(PCACHE_TESTS
    happy_path_single
    happy_path_multiple
    fixed_policy
    fifo_policy
    edge_cases
    elasticity
    drafragmentation
    database_check
)

foreach(test_name ${PCACHE_TESTS})
    add_executable(${test_name} ${test_name}.c)
    target_link_libraries(${test_name} pcache_test_common pcache ${SQLITE3_LIBRARIES})
    add_test(NAME ${test_name} COMMAND ${test_name} --report-error)
endforeach()

# Unit tests for internal helpers — needs access to src/ private headers.
add_executable(unitary unitary.c)
target_include_directories(unitary PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(unitary pcache ${SQLITE3_LIBRARIES})
add_test(NAME unitary COMMAND unitary --report-error)
