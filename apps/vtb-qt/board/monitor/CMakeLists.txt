cmake_minimum_required(VERSION 3.16)
project(vtb_board_monitor LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "vtb-board-monitor reads Linux kernel interfaces")
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json 3.11.3 QUIET)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
    include(FetchContent)
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    set(JSON_Install OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_executable(vtb-board-monitor main.cc details.cc)
target_compile_features(vtb-board-monitor PRIVATE cxx_std_20)
target_compile_options(vtb-board-monitor PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(vtb-board-monitor PRIVATE nlohmann_json::nlohmann_json)
install(TARGETS vtb-board-monitor RUNTIME DESTINATION bin)
