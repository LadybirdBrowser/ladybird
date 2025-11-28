set(SOURCES
    Cache/CacheEntry.cpp
    Cache/CacheIndex.cpp
    Cache/DiskCache.cpp
    Cache/Utilities.cpp
    Header.cpp
    HeaderList.cpp
    HTTP.cpp
    HttpRequest.cpp
    Method.cpp
)

ladybird_lib(LibHTTP http)
target_link_libraries(LibHTTP PRIVATE LibCompress LibCore LibCrypto LibDatabase LibFileSystem LibIPC LibRegex LibTextCodec LibTLS LibURL)
