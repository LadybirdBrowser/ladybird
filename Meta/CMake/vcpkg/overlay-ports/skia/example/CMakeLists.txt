cmake_minimum_required(VERSION 3.7)

project(skia-example)

set(CMAKE_CXX_STANDARD 17)

add_executable(legacy convert-to-nia.cpp)
find_package(skia CONFIG REQUIRED)
target_link_libraries(legacy PRIVATE skia)

find_package(unofficial-skia CONFIG REQUIRED)

add_executable(main convert-to-nia.cpp)
target_link_libraries(main PRIVATE unofficial::skia::skia unofficial::skia::modules::skshaper)

add_executable(skcms skcms.cpp)
target_link_libraries(skcms PRIVATE unofficial::skia::skia)

add_executable(skpathediting skpathediting.cpp)
target_link_libraries(skpathediting PRIVATE unofficial::skia::skia)
