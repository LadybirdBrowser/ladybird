set(SOURCES
    Deflate.cpp
    PackBitsDecoder.cpp
    Zlib.cpp
    Gzip.cpp
    GenericZlib.cpp
)

serenity_lib(LibCompress compress)
target_link_libraries(LibCompress PRIVATE LibCore LibCrypto)

find_package(ZLIB REQUIRED)
target_link_libraries(LibCompress PRIVATE ZLIB::ZLIB)
