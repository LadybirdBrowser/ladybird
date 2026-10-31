set(SOURCES
    GenericZlib.cpp
    Zlib.cpp
)

ladybird_lib(LibCompress compress)
target_link_libraries(LibCompress PRIVATE ZLIB::ZLIB)
