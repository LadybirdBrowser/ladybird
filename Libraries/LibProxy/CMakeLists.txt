set(SOURCES
    Proxy.cpp
)

if (APPLE)
    list(APPEND SOURCES ProxyMacOS.cpp)
endif()

ladybird_lib(LibProxy proxy)

target_link_libraries(LibProxy PUBLIC LibCore LibURL LibUnicode)

if (APPLE)
    target_link_libraries(LibProxy PRIVATE "-framework CoreFoundation")
    target_link_libraries(LibProxy PRIVATE "-framework SystemConfiguration")
endif()
