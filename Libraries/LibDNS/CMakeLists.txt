set(SOURCES
    Message.cpp
)

ladybird_lib(LibDNS dns)
target_link_libraries(LibDNS PRIVATE LibCore PUBLIC LibCrypto)
