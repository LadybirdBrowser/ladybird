set(SOURCES
    Deflate.cpp
    GenericZlib.cpp
    Gzip.cpp
    PackBitsDecoder.cpp
    Zlib.cpp
    Zstd.cpp
)

find_package(zstd CONFIG REQUIRED)

ladybird_lib(LibCompress compress)
target_link_libraries(LibCompress PRIVATE LibCore LibCrypto)

target_link_libraries(LibCompress PRIVATE ZLIB::ZLIB zstd::libzstd)
