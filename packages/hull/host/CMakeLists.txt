cmake_minimum_required(VERSION 3.16)
project(hull_host LANGUAGES C CXX)   # C needed for the vendored SQLite amalgamation

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(WEBVIEW_WEBKITGTK_API 6.0)   # use WebKitGTK 6 on Linux

if (MSVC)
  add_compile_options(/utf-8 /EHsc)
endif()

include(FetchContent)

FetchContent_Declare(webview
  GIT_REPOSITORY https://github.com/webview/webview
  GIT_TAG 0.12.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(webview)

FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(json)

FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib
  GIT_TAG v0.18.3
  GIT_SHALLOW TRUE)
set(HTTPLIB_REQUIRE_OPENSSL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)   # SQLite (THREADSAFE) needs pthread on POSIX

# --- Security build option ---
# OFF (default): NullCipher (fast, plaintext at rest) + vendored vanilla SQLite.
# ON:            AES-256-GCM for files/settings + SQLCipher for the DB; the binary is
#                named hull-host-secure. Requires SQLCipher (Linux: libsqlcipher-dev;
#                Windows: vcpkg install sqlcipher; macOS: brew install sqlcipher).
option(HULL_CRYPTO "Encrypt the database (SQLCipher) and files/settings (AES) at rest" OFF)

set(SQLITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite")

# WIN32 => use WinMain on Windows (no console window). The host loads its UI at
# runtime (--url / --app), so there is no embedded index_html.h.
add_executable(hull-host WIN32)
target_sources(hull-host PRIVATE src/main.cpp)
target_include_directories(hull-host PRIVATE src)

target_link_libraries(hull-host PRIVATE
  webview::core
  nlohmann_json::nlohmann_json
  httplib::httplib
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads
  ${CMAKE_DL_LIBS})

# --- SQLite backend: vendored vanilla (default) or SQLCipher (secure build) ---
if (HULL_CRYPTO)
  set_target_properties(hull-host PROPERTIES OUTPUT_NAME "hull-host-secure")
  target_compile_definitions(hull-host PRIVATE HULL_CRYPTO)
  if (WIN32)
    find_package(unofficial-sqlcipher CONFIG REQUIRED)  # vcpkg: sqlcipher
    target_link_libraries(hull-host PRIVATE unofficial::sqlcipher::sqlcipher)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SQLCIPHER REQUIRED sqlcipher)      # apt: libsqlcipher-dev / brew: sqlcipher
    target_include_directories(hull-host PRIVATE ${SQLCIPHER_INCLUDE_DIRS})
    target_link_libraries(hull-host PRIVATE ${SQLCIPHER_LIBRARIES})
  endif()
else()
  # Vendored vanilla SQLite amalgamation (no system dependency). Hardened build:
  # no extension loading, strict string literals, foreign keys on, FTS5, thread-safe.
  target_sources(hull-host PRIVATE "${SQLITE_DIR}/sqlite3.c")
  target_include_directories(hull-host PRIVATE "${SQLITE_DIR}")
  set_source_files_properties("${SQLITE_DIR}/sqlite3.c" PROPERTIES COMPILE_DEFINITIONS
    "SQLITE_THREADSAFE=1;SQLITE_DQS=0;SQLITE_OMIT_LOAD_EXTENSION=1;SQLITE_DEFAULT_FOREIGN_KEYS=1;SQLITE_ENABLE_FTS5=1;SQLITE_DEFAULT_MEMSTATUS=0;SQLITE_OMIT_DEPRECATED=1")
endif()

# --- Platform: keychain + printing + sockets + (Gdiplus for the window icon) ---
if (WIN32)
  target_link_libraries(hull-host PRIVATE Advapi32 Winspool Crypt32 ws2_32 Gdiplus Gdi32 User32)
elseif (APPLE)
  find_library(SECURITY_FRAMEWORK Security)
  find_library(CF_FRAMEWORK CoreFoundation)
  find_library(CUPS_LIB cups)
  target_link_libraries(hull-host PRIVATE ${SECURITY_FRAMEWORK} ${CF_FRAMEWORK} ${CUPS_LIB})
else() # Linux
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SECRET REQUIRED libsecret-1)
  pkg_check_modules(CUPS REQUIRED cups)
  pkg_check_modules(GLIB REQUIRED glib-2.0)   # g_set_prgname: window app-id -> desktop icon
  target_include_directories(hull-host PRIVATE ${SECRET_INCLUDE_DIRS} ${CUPS_INCLUDE_DIRS} ${GLIB_INCLUDE_DIRS})
  target_link_libraries(hull-host PRIVATE ${SECRET_LIBRARIES} ${CUPS_LIBRARIES} ${GLIB_LIBRARIES})
endif()
