# k4_gotv — Plugify C++ port of K4ryuu/K4-GOTV (SwiftlyS2, C#): automatic GOTV recording,
# per-round cropping, the !demo request command, Discord webhook notifications, MySQL logging and
# local cleanup/retention. FTP/SFTP/Mega.nz are replaced by an S3-compatible uploader
# (AWS Signature V4, single PUT / multipart, ListObjectsV2 retention).
#
# Pure s2sdk + plgdb (docs/PLUGIN_STANDARD.md §7): no pisexcompat, no hudmenu dependency and no
# other shared wrapper library. The map gate (PITFALLS B1/B2), the client/controller/handle guards
# (C1/C2), the map-gated schema lookup (C8), the update-driven timers (C9), the event hooks and
# the `demo` command with their symmetric teardown (C11/C13/A7) all live in src/.
#
#   k4_gotv_core        engine-free core (config JSONC parsing, naming, time formats, SHA-256 /
#                       HMAC / SigV4, S3 XML helpers, upload planning, retention selection,
#                       ZIP writer, Discord payload/multipart, the k4-gotv table SQL and its
#                       parameters, the colour/language/log-redaction text helpers and the KV1
#                       reader for databases.cfg; used by tests/k4_gotv_tests.cpp)
#   plugify-k4_gotv     the plugin shared library (entry "bin/k4_gotv")

set(K4_GOTV_VERSION "1.2.0")

# The monorepo root sets this; a standalone configure of this directory (the exported repository)
# must compile the same C++23 sources without depending on the root file.
if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# --- generated bindings / SDK / plgdb -----------------------------------------
# The __s2sdk_* binding globals have to be compiled into every plugin; the root CMakeLists builds
# them as the object library `s2sdk_bindings`. These are the standalone fallbacks for a configure
# that only includes this directory - named differently on purpose, so they can never collide with
# the root targets when the whole tree is configured.
set(K4_GOTV_SDK_INCLUDE "${CMAKE_SOURCE_DIR}/sdk/include")
if(NOT EXISTS "${K4_GOTV_SDK_INCLUDE}/s2sdk/s2sdk.cpp")
    # A configure of just plugins/k4_gotv inside the monorepo: the shared SDK is two levels up.
    set(K4_GOTV_SDK_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/include")
endif()
if(NOT EXISTS "${K4_GOTV_SDK_INCLUDE}/s2sdk/s2sdk.cpp")
    message(FATAL_ERROR "k4_gotv needs the generated s2sdk bindings (sdk/include/s2sdk/s2sdk.cpp)")
endif()

if(TARGET plugify_sdk)
    set(K4_GOTV_PLUGIFY_SDK plugify_sdk)
else()
    add_library(k4_gotv_plugify_sdk INTERFACE)
    target_include_directories(k4_gotv_plugify_sdk INTERFACE "${K4_GOTV_SDK_INCLUDE}")
    set(K4_GOTV_PLUGIFY_SDK k4_gotv_plugify_sdk)
endif()

if(TARGET s2sdk_bindings)
    set(K4_GOTV_S2SDK_BINDINGS "$<TARGET_OBJECTS:s2sdk_bindings>")
else()
    add_library(k4_gotv_s2sdk_bindings OBJECT "${K4_GOTV_SDK_INCLUDE}/s2sdk/s2sdk.cpp")
    target_include_directories(k4_gotv_s2sdk_bindings SYSTEM PUBLIC "${K4_GOTV_SDK_INCLUDE}")
    target_compile_definitions(k4_gotv_s2sdk_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(K4_GOTV_S2SDK_BINDINGS "$<TARGET_OBJECTS:k4_gotv_s2sdk_bindings>")
endif()

# plgdb is the one allowed library for plugins that use a database (PLUGIN_STANDARD.md §7). The
# root CMakeLists adds it before the plugin directories; a standalone configure of this directory
# adds the checked-in copy itself.
if(TARGET plgdb)
    set(K4_GOTV_PLGDB plgdb)
else()
    set(K4_GOTV_PLGDB_DIR "")
    foreach(_candidate "${CMAKE_SOURCE_DIR}/libs/plgdb" "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/plgdb")
        if(EXISTS "${_candidate}/CMakeLists.txt")
            set(K4_GOTV_PLGDB_DIR "${_candidate}")
            break()
        endif()
    endforeach()
    if(K4_GOTV_PLGDB_DIR STREQUAL "")
        message(FATAL_ERROR "k4_gotv needs libs/plgdb (the async MySQL/SQLite library)")
    endif()
    add_subdirectory("${K4_GOTV_PLGDB_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/plgdb")
    set(K4_GOTV_PLGDB plgdb)
endif()

set(K4_GOTV_PLGDB_INCLUDE "${CMAKE_SOURCE_DIR}/libs/plgdb/include")
if(NOT EXISTS "${K4_GOTV_PLGDB_INCLUDE}/plgdb/db.hpp")
    set(K4_GOTV_PLGDB_INCLUDE "${K4_GOTV_PLGDB_DIR}/include")
endif()
if(NOT EXISTS "${K4_GOTV_PLGDB_INCLUDE}/plgdb/db.hpp")
    message(FATAL_ERROR "k4_gotv needs the plgdb headers")
endif()

# --- core ---------------------------------------------------------------------
add_library(k4_gotv_core STATIC
        src/k4_core.cpp
        src/k4_crypto.cpp
        src/k4_sigv4.cpp
        src/k4_s3sign.cpp
        src/k4_s3util.cpp
        src/k4_sql.cpp
        src/k4_zip.cpp
        src/k4_discord_payload.cpp
        src/k4_curl.cpp
        src/k4_text.cpp
        src/k4_kv1.cpp
)
target_include_directories(k4_gotv_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: the vendored nlohmann/json must not fail -Werror on our sources.
target_include_directories(k4_gotv_core SYSTEM PRIVATE "${K4_GOTV_SDK_INCLUDE}")
# k4_sql.hpp exposes plgdb::Params (header-only in the core; the plugin links plgdb itself).
target_include_directories(k4_gotv_core SYSTEM PRIVATE "${K4_GOTV_PLGDB_INCLUDE}")
target_compile_options(k4_gotv_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(k4_gotv_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
# k4_curl.cpp loads libcurl with dlopen at runtime (no DT_NEEDED entry, no dev headers needed).
target_link_libraries(k4_gotv_core PUBLIC ${CMAKE_DL_LIBS})

# --- plugin -------------------------------------------------------------------
add_library(plugify-k4_gotv SHARED
        src/plugin.cpp
        src/k4_http.cpp
        src/k4_worker.cpp
        src/k4_s3.cpp
        src/k4_discord.cpp
        src/k4_database.cpp
        # Binding globals (__s2sdk_*) filled by the plugify module from the plugins declared as
        # dependencies.
        ${K4_GOTV_S2SDK_BINDINGS}
)
target_include_directories(plugify-k4_gotv PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated s2sdk/plg/plgdb headers must not fail -Werror.
target_include_directories(plugify-k4_gotv SYSTEM PRIVATE
        "${K4_GOTV_SDK_INCLUDE}"
        "${K4_GOTV_PLGDB_INCLUDE}")
target_link_libraries(plugify-k4_gotv PRIVATE
        k4_gotv_core
        ${K4_GOTV_PLGDB}
        ${K4_GOTV_PLUGIFY_SDK}
        -static-libstdc++ -static-libgcc)
target_compile_options(plugify-k4_gotv PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-k4_gotv PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="k4_gotv"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-k4_gotv PROPERTIES OUTPUT_NAME k4_gotv)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-k4_gotv PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/k4_gotv.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/k4_gotv.pplugin"
)

# --- tests --------------------------------------------------------------------
# The top-level CMakeLists declares PLUGIFY_PORT_TESTS *after* the plugin directories, so on a
# first configure it is not defined here yet: default it to the same ON. An explicit
# -DPLUGIFY_PORT_TESTS=OFF (the exported standalone build) is already in the cache and still wins.
if(NOT DEFINED PLUGIFY_PORT_TESTS)
    set(PLUGIFY_PORT_TESTS ON)
endif()
if(PLUGIFY_PORT_TESTS)
# Pure logic: SHA-256/HMAC/CRC32, AWS SigV4 test vectors, canonical request/URI encoding, config
# parsing/defaults, multipart part planning, retention selection, naming, S3 XML, ZIP structure,
# Discord payload/multipart, the colour/language/log-redaction helpers, the KV1 reader, the map
# gate/handle rules and the `k4-gotv` table statements against SQLite (and against the throwaway
# MariaDB when ./test.sh exports MYSQL_HOST/MYSQL_DATABASE/MYSQL_USER/MYSQL_PASSWORD).
find_package(Threads REQUIRED)
add_executable(k4_gotv_tests tests/k4_gotv_tests.cpp tests/k4_s3_tests.cpp tests/k4_sql_tests.cpp
        src/k4_s3.cpp src/k4_worker.cpp)
target_include_directories(k4_gotv_tests SYSTEM PRIVATE "${K4_GOTV_SDK_INCLUDE}")
# Threads: k4_worker.cpp runs the S3 state-machine test's file reads on a std::thread.
# plgdb: the table statements are executed against a real in-memory SQLite database.
target_link_libraries(k4_gotv_tests PRIVATE k4_gotv_core ${K4_GOTV_PLGDB} Threads::Threads
        -static-libstdc++ -static-libgcc)
target_compile_options(k4_gotv_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(k4_gotv_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        # Lets the tests parse the shipped config.jsonc, so a broken package asset fails CI.
        K4_GOTV_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
set_target_properties(k4_gotv_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# HTTP service plumbing: the real k4_http.cpp with no Steam transport in the process, so every
# request must run over libcurl; checks completion delivery, FIFO order and a prompt Shutdown.
add_executable(k4_gotv_http_tests tests/k4_http_tests.cpp src/k4_http.cpp ${K4_GOTV_S2SDK_BINDINGS})
target_include_directories(k4_gotv_http_tests SYSTEM PRIVATE "${K4_GOTV_SDK_INCLUDE}")
target_link_libraries(k4_gotv_http_tests PRIVATE
        k4_gotv_core Threads::Threads -static-libstdc++ -static-libgcc)
target_compile_options(k4_gotv_http_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(k4_gotv_http_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
set_target_properties(k4_gotv_http_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# Fake-runtime test: the real src/plugin.cpp is compiled together with the generated binding
# objects, whose __s2sdk_* globals are null until the test installs recording fakes. It drives the
# plugin through its own exported entry points and proves the map gate (B1/B2), the command/event
# teardown (A7/C11/C13), the player-slot accessor (C13), the update-driven timers (C9), the handle
# rules (C1/C2) and the database/config path priority.
add_executable(k4_gotv_runtime_tests
        tests/k4_gotv_runtime_tests.cpp
        src/plugin.cpp
        src/k4_http.cpp
        src/k4_worker.cpp
        src/k4_s3.cpp
        src/k4_discord.cpp
        src/k4_database.cpp
        ${K4_GOTV_S2SDK_BINDINGS}
)
target_include_directories(k4_gotv_runtime_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(k4_gotv_runtime_tests SYSTEM PRIVATE
        "${K4_GOTV_SDK_INCLUDE}"
        "${K4_GOTV_PLGDB_INCLUDE}")
target_link_libraries(k4_gotv_runtime_tests PRIVATE
        k4_gotv_core ${K4_GOTV_PLGDB} ${K4_GOTV_PLUGIFY_SDK} Threads::Threads
        -static-libstdc++ -static-libgcc)
target_compile_options(k4_gotv_runtime_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(k4_gotv_runtime_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="k4_gotv"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1)
set_target_properties(k4_gotv_runtime_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# Convenience for iterating inside the builder container (./build.sh k4_gotv_run_tests). The
# normal entry point is ./test.sh, which picks up build/tests/k4_gotv_tests like every other
# ported plugin. EXCLUDE_FROM_ALL keeps a plain `ninja` from running the tests.
add_custom_target(k4_gotv_run_tests
        COMMAND $<TARGET_FILE:k4_gotv_tests>
        COMMAND $<TARGET_FILE:k4_gotv_runtime_tests>
        DEPENDS k4_gotv_tests k4_gotv_runtime_tests
        COMMENT "Running k4_gotv_tests and k4_gotv_runtime_tests")
set_target_properties(k4_gotv_run_tests PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# --- host-side S3 probe -------------------------------------------------------------------------
# Sends the same signed requests the plugin sends, through libcurl loaded at runtime, so the lead
# can verify the endpoint/signature/headers from the host with the real keys:
#
#   AWS_ACCESS_KEY_ID=... AWS_SECRET_ACCESS_KEY=... build/tools/k4_gotv_s3probe
#   AWS_ACCESS_KEY_ID=... AWS_SECRET_ACCESS_KEY=... build/tools/k4_gotv_s3probe --multipart
#
# EXCLUDE_FROM_ALL: a plain `ninja` does not build it; use ./build.sh k4_gotv_s3probe.
# The exported plugin repository does not carry the plugin's development tools/ directory, so the
# probe is only configured when its source is present.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/s3probe.cpp")
    add_executable(k4_gotv_s3probe tools/s3probe.cpp)
    target_link_libraries(k4_gotv_s3probe PRIVATE k4_gotv_core -static-libstdc++ -static-libgcc)
    target_compile_options(k4_gotv_s3probe PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(k4_gotv_s3probe PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
    set_target_properties(k4_gotv_s3probe PROPERTIES
            EXCLUDE_FROM_ALL TRUE
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
endif()
