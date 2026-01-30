set(SOURCES
    Deflate.cpp
    GenericZlib.cpp
    Gzip.cpp
    PackBitsDecoder.cpp
    Zlib.cpp
)

ladybird_lib(LibCompress compress)
target_link_libraries(LibCompress PRIVATE LibCore LibCrypto)

set_target_properties(LibCompress PROPERTIES UNITY_BUILD ${LADYBIRD_ENABLE_UNITY_BUILD})

find_package(ZLIB REQUIRED)
target_link_libraries(LibCompress PRIVATE ZLIB::ZLIB)
