duckdb_extension_load(yaml
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/src/include
    LINKED_LIBS "$<TARGET_FILE:yaml-cpp::yaml-cpp>"
    LOAD_TESTS
    TEST_DIR ${CMAKE_CURRENT_LIST_DIR}/test
)
duckdb_extension_load(json)
