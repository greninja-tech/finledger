cmake_minimum_required(VERSION 3.24)
project(FinLedgerBackend VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.3
    GIT_SHALLOW TRUE)
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(httplib json)

add_executable(finledger_server src/main.cpp)
target_link_libraries(finledger_server PRIVATE httplib::httplib nlohmann_json::nlohmann_json)
target_include_directories(finledger_server PRIVATE include)
if(MSVC)
    target_compile_options(finledger_server PRIVATE /W4 /permissive-)
    target_compile_definitions(finledger_server PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(finledger_server PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Independent, dependency-light systems-engineering lab. This target exercises
# the educational LSM/MVCC/sharding/Raft modules separately from the HTTP API.
add_executable(finledger_systems_lab systems_lab/src/systems_lab.cpp)
target_include_directories(finledger_systems_lab PRIVATE systems_lab/include)
if(MSVC)
    target_compile_options(finledger_systems_lab PRIVATE /W4 /permissive-)
else()
    target_compile_options(finledger_systems_lab PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(finledger_systems_lab_test systems_lab/tests/systems_lab_test.cpp)
    target_include_directories(finledger_systems_lab_test PRIVATE systems_lab/include)
    add_test(NAME finledger_systems_lab_test COMMAND finledger_systems_lab_test)
endif()
