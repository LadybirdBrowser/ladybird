set(SOURCES
    Cache/CacheEntry.cpp
    Cache/CacheIndex.cpp
    Cache/DiskCache.cpp
    Cache/DiskCacheSettings.cpp
    Cache/MemoryCache.cpp
    Cache/Utilities.cpp
    Cookie/Cookie.cpp
    Cookie/ParsedCookie.cpp
    Header.cpp
    HeaderList.cpp
    HTTP.cpp
    HttpRequest.cpp
    Method.cpp
)

ladybird_lib(LibHTTP http)
target_link_libraries(LibHTTP PRIVATE LibCompress LibCore LibCrypto LibDatabase LibFileSystem LibIPC LibRegex LibTextCodec LibTLS LibUnicode LibURL)
