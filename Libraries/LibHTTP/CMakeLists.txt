set(SOURCES
    Header.cpp
    HeaderList.cpp
    HTTP.cpp
    HttpRequest.cpp
    Method.cpp
)

ladybird_lib(LibHTTP http)
target_link_libraries(LibHTTP PRIVATE LibCompress LibCore LibIPC LibRegex LibTextCodec LibTLS LibURL)
