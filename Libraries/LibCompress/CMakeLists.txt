set(SOURCES
    Brotli.cpp
    Deflate.cpp
    GenericZlib.cpp
    Gzip.cpp
    Zlib.cpp
)

ladybird_lib(LibCompress compress)
target_link_libraries(LibCompress PRIVATE LibCore LibCrypto)

target_link_libraries(LibCompress PRIVATE ZLIB::ZLIB)
target_link_libraries(LibCompress PRIVATE ${BROTLI_TARGETS})
