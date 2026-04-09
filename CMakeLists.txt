cmake_minimum_required(VERSION 3.16)
project(apiexec VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Sanitiser option
option(SANITIZE "Enable ASAN + UBSAN" OFF)
if(SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Dependencies
find_package(CURL REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)

# ---------------------------------------------------------------------------
# Layer 1: core/ — header-only interfaces and value types. Zero external deps.
# ---------------------------------------------------------------------------
add_library(apiexec_core INTERFACE)
target_include_directories(apiexec_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------
# Layer 2: transport/ — libcurl behind ITransport. Depends on core/ only.
# ---------------------------------------------------------------------------
add_library(apiexec_transport
    transport/curl_transport.cpp
)
target_link_libraries(apiexec_transport PUBLIC apiexec_core PRIVATE CURL::libcurl)

# ---------------------------------------------------------------------------
# Layer 3: policy/ — DefaultPolicy. Depends on core/ only.
# ---------------------------------------------------------------------------
add_library(apiexec_policy
    policy/default_policy.cpp
)
target_link_libraries(apiexec_policy PUBLIC apiexec_core)

# ---------------------------------------------------------------------------
# Layer 4: adapters/ — vendor adapters + registry. Depends on core/ only
#          (plus nlohmann/json and curl escape utility as external deps).
# ---------------------------------------------------------------------------
add_library(apiexec_adapters
    adapters/generic_rest.cpp
    adapters/registry.cpp
)
target_link_libraries(apiexec_adapters
    PUBLIC apiexec_core
    PRIVATE nlohmann_json::nlohmann_json CURL::libcurl
)

# ---------------------------------------------------------------------------
# Convenience target: links all layers for binaries that need everything.
# ---------------------------------------------------------------------------
add_library(apiexec_all INTERFACE)
target_link_libraries(apiexec_all INTERFACE
    apiexec_core
    apiexec_transport
    apiexec_policy
    apiexec_adapters
)

# ---------------------------------------------------------------------------
# Main executable (smoke test driver)
# ---------------------------------------------------------------------------
add_executable(apiexec main.cpp)
target_link_libraries(apiexec PRIVATE apiexec_all)

# ---------------------------------------------------------------------------
# Testing
# ---------------------------------------------------------------------------
enable_testing()

# Unit tests — link only the layers they actually need.
# cursor_test: core/ only (header-only, no link deps)
add_executable(cursor_test ../tests/unit/cursor_test.cpp)
target_link_libraries(cursor_test PRIVATE apiexec_core)
add_test(NAME cursor_test COMMAND cursor_test)

# policy_test: core/ + policy/
add_executable(policy_test ../tests/unit/policy_test.cpp)
target_link_libraries(policy_test PRIVATE apiexec_policy)
add_test(NAME policy_test COMMAND policy_test)

# retry_test: core/ + policy/ (mock transport, no curl needed)
add_executable(retry_test ../tests/unit/retry_test.cpp)
target_link_libraries(retry_test PRIVATE apiexec_policy)
add_test(NAME retry_test COMMAND retry_test)

# exhaustion_test: core/ + policy/ (mock transport, no curl needed)
add_executable(exhaustion_test ../tests/unit/exhaustion_test.cpp)
target_link_libraries(exhaustion_test PRIVATE apiexec_policy)
add_test(NAME exhaustion_test COMMAND exhaustion_test)

# engine_test: all layers (uses GenericRestAdapter + nlohmann/json)
add_executable(engine_test ../tests/unit/engine_test.cpp)
target_link_libraries(engine_test PRIVATE apiexec_all nlohmann_json::nlohmann_json)
add_test(NAME engine_test COMMAND engine_test)

# Integration tests
set(MOCK_SERVER_SRC ../tests/integration/mock_server/mock_server.cpp)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tests/integration/e2e_test.cpp")
    add_executable(e2e_test ../tests/integration/e2e_test.cpp ${MOCK_SERVER_SRC})
    target_include_directories(e2e_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tests/integration)
    target_link_libraries(e2e_test PRIVATE apiexec_all nlohmann_json::nlohmann_json)
    add_test(NAME e2e_test COMMAND e2e_test)
endif()
