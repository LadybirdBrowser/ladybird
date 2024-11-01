set(SOURCES
    Message.cpp
)

serenity_lib(LibDNS dns)
target_link_libraries(LibDNS PRIVATE LibCore)
