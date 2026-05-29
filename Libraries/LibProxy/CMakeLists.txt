set(SOURCES
    Proxy.cpp
    ProxyManager.cpp
    Platform/ProxyPlatform.cpp
    Platform/ProxyEnv.cpp
    Platform/ProxyEnv.cpp
)

if (APPLE)
    list(APPEND SOURCES Platform/ProxyMac.cpp)
endif()

ladybird_lib(LibProxy proxy)

target_link_libraries(LibProxy PUBLIC LibCore LibURL LibUnicode LibSync)

if (APPLE)
    target_link_libraries(LibProxy PRIVATE "-framework CoreFoundation")
    target_link_libraries(LibProxy PRIVATE "-framework SystemConfiguration")
endif()
