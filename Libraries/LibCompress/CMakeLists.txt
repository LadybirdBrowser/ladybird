set(SOURCES
    Deflate.cpp
    GenericZlib.cpp
    Gzip.cpp
    PackBitsDecoder.cpp
    Zlib.cpp
)

ladybird_lib(LibCompress compress)
target_link_libraries(LibCompress PRIVATE LibCore LibCrypto)

find_package(ZLIB REQUIRED)
target_link_libraries(LibCompress PRIVATE ZLIB::ZLIB)

if(ENABLE_UNITY_BUILD)
    set_target_properties(LibCompress PROPERTIES UNITY_BUILD ON)
endif()
