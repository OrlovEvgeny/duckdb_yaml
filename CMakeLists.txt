cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME yaml)

if(NOT TARGET yaml-cpp::yaml-cpp)
  find_package(yaml-cpp REQUIRED)
endif()

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include ${DUCKDB_MODULE_BASE_DIR}/extension/json/include)

set(EXTENSION_SOURCES
  src/yaml_extension.cpp
  src/yaml_reader.cpp
  src/yaml_reader_types.cpp
  src/yaml_reader_parsing.cpp
  src/yaml_reader_files.cpp
  src/yaml_reader_functions.cpp
  src/yaml_reader_column_bind.cpp
  src/yaml_frontmatter.cpp
  src/yaml_types.cpp
  src/yaml_column_types.cpp
  src/yaml_scalar_functions.cpp
  src/yaml_extraction_functions.cpp
  src/yaml_unnest_functions.cpp
  src/yaml_copy.cpp
  src/yaml_utils.cpp
  src/yaml_formatting.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})
# Windows MSVC needs /std:c++17 to compile duckdb's vendored fmt/format.h
# (which uses C++17 inline variables). MSVC defaults to /std:c++14. duckdb's
# own Windows build must already use C++17 since it vendors fmt and links it
# into duckdb_static; aligning our extension targets here keeps the standard
# consistent across the link. Not applied on Linux: forcing C++17 on our
# extension there causes static-const data members in duckdb's headers to
# acquire implicit inline linkage in our TUs but not in libduckdb's
# C++11-built TUs, producing multiple-definition link errors.
if(MSVC)
    target_compile_options(${EXTENSION_NAME} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/std:c++17>")
    target_compile_options(${LOADABLE_EXTENSION_NAME} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/std:c++17>")
endif()


target_link_libraries(${EXTENSION_NAME} yaml-cpp::yaml-cpp json_extension)
target_link_libraries(${LOADABLE_EXTENSION_NAME} yaml-cpp::yaml-cpp json_extension)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

option(YAML_ALLOW_GLOBAL_SETTERS "Register process-wide YAML setters" OFF)
if(NOT YAML_ALLOW_GLOBAL_SETTERS)
  target_compile_definitions(${EXTENSION_NAME} PRIVATE YAML_DISABLE_GLOBAL_SETTERS)
  target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE YAML_DISABLE_GLOBAL_SETTERS)
endif()
