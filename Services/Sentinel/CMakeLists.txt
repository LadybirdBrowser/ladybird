set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

set(SOURCES
    PolicyGraph.cpp
    SentinelServer.cpp
)

add_library(sentinelservice STATIC ${SOURCES})

add_executable(Sentinel main.cpp)
add_executable(TestPolicyGraph TestPolicyGraph.cpp)

target_include_directories(sentinelservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(sentinelservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

# Find YARA library
find_library(YARA_LIBRARY NAMES yara libyara PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib)
find_path(YARA_INCLUDE_DIR yara.h PATHS /usr/include /usr/local/include)

if(NOT YARA_LIBRARY)
    message(WARNING "YARA library not found. Sentinel will not build.")
    message(WARNING "Install YARA with: sudo apt-get install libyara-dev yara")
else()
    message(STATUS "Found YARA library: ${YARA_LIBRARY}")
    message(STATUS "Found YARA include: ${YARA_INCLUDE_DIR}")
    target_include_directories(sentinelservice PRIVATE ${YARA_INCLUDE_DIR})
    target_link_libraries(sentinelservice PRIVATE ${YARA_LIBRARY})
endif()

target_link_libraries(sentinelservice PRIVATE LibCore LibDatabase LibFileSystem LibIPC)
target_link_libraries(Sentinel PRIVATE sentinelservice LibCore LibMain)
target_link_libraries(TestPolicyGraph PRIVATE sentinelservice LibCore LibMain)
