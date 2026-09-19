set(SOURCES
    Message.cpp
    Validator.cpp
)

ladybird_lib(LibDNS dns EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibDNS PRIVATE LibCore PUBLIC LibCrypto LibThreading)
