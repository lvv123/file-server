cmake_minimum_required(VERSION 3.20)
project(file_server LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN _WINSOCK_DEPRECATED_NO_WARNINGS)
  add_compile_options(/utf-8 /W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------- sqlite3
add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)
target_compile_definitions(sqlite3 PRIVATE
  SQLITE_THREADSAFE=1
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_DQS=0
  SQLITE_DEFAULT_MEMSTATUS=0)
if(MSVC)
  target_compile_options(sqlite3 PRIVATE /W0)
  target_compile_definitions(sqlite3 PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(sqlite3 PRIVATE -w)
endif()

# ---------------------------------------------------------------- core
set(CORE_SOURCES
  src/util.cpp
  src/config.cpp
  src/paths.cpp
  src/fsops.cpp
  src/db.cpp
  src/repo_files.cpp
  src/repo_sessions.cpp
  src/repo_audit.cpp
  src/audit.cpp
  src/range.cpp
  src/upload.cpp
  src/download.cpp
  src/handler_common.cpp
  src/handler_files.cpp
  src/handler_upload.cpp
  src/handler_admin.cpp
  src/server.cpp
  src/static_assets.cpp)

add_library(fs_core STATIC ${CORE_SOURCES})
target_include_directories(fs_core PUBLIC src third_party)
target_link_libraries(fs_core PUBLIC sqlite3)
if(WIN32)
  target_link_libraries(fs_core PUBLIC ws2_32)
endif()

# ---------------------------------------------------------------- app
add_executable(file-server src/main.cpp src/app.manifest)
target_link_libraries(file-server PRIVATE fs_core)
set_target_properties(file-server PROPERTIES OUTPUT_NAME "file-server")

# ---------------------------------------------------------------- tests
enable_testing()
file(GLOB UNIT_TEST_SOURCES tests/unit/*.cpp)
add_executable(fs_unit_tests ${UNIT_TEST_SOURCES})
target_include_directories(fs_unit_tests PRIVATE tests/unit)
target_link_libraries(fs_unit_tests PRIVATE fs_core)
add_test(NAME unit COMMAND fs_unit_tests)

# Optional: regenerate embedded web assets from web/ at build time.
option(FS_REGENERATE_ASSETS "Regenerate src/static_assets.cpp from web/ using Python" OFF)
if(FS_REGENERATE_ASSETS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/static_assets.cpp
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_assets.py
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html
            ${CMAKE_CURRENT_SOURCE_DIR}/web/app.js
            ${CMAKE_CURRENT_SOURCE_DIR}/web/style.css
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_assets.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Regenerating src/static_assets.cpp")
endif()
