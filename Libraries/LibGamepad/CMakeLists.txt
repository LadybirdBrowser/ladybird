set(SOURCES
    SDLGamepadSource.cpp
)

ladybird_lib(LibGamepad gamepad)
target_link_libraries(LibGamepad PRIVATE LibCore LibWebCommon LibWebView SDL3::SDL3)
