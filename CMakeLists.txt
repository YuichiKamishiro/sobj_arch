cmake_minimum_required(VERSION 3.10...4.0)
project(sobjectizer_setup LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(sobjectizer CONFIG REQUIRED)
find_package(nlohmann_json 3.12.0 REQUIRED)

add_subdirectory(tester)


add_executable(run src/main.cpp)

target_link_libraries(run PRIVATE sobjectizer::StaticLib nlohmann_json::nlohmann_json)