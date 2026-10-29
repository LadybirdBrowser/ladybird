ladybird_test_suite(LibSandbox)
target_link_libraries(TestLibSandbox PRIVATE LibSandbox)

if (LINUX)
    # Applies the real sandbox of the image decoder, so it builds its sandbox source.
    target_sources(TestLibSandbox PRIVATE
        TestImageDecoderSandbox.cpp
        TestLandlock.cpp
        TestSeccomp.cpp
        ${LADYBIRD_SOURCE_DIR}/Services/ImageDecoder/SandboxLinux.cpp
    )
    target_include_directories(TestLibSandbox PRIVATE ${LADYBIRD_SOURCE_DIR}/Services)
endif()

if (APPLE)
    # Applies the real sandboxes of the helpers, so it builds their sandbox sources.
    target_sources(TestLibSandbox PRIVATE
        TestHelperSandboxes.cpp
        TestSeatbelt.cpp
        ${LADYBIRD_SOURCE_DIR}/Services/Compositor/SandboxMacOS.cpp
        ${LADYBIRD_SOURCE_DIR}/Services/RendererSandboxMacOS.cpp
    )
    target_include_directories(TestLibSandbox PRIVATE ${LADYBIRD_SOURCE_DIR}/Services)
    target_link_libraries(TestLibSandbox PRIVATE LibWebView
        "-framework CoreFoundation" "-framework CoreServices" "-framework IOKit"
        "-framework IOSurface" "-framework Security" "-framework CoreGraphics"
    )
endif()
