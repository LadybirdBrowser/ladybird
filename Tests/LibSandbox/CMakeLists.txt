if (LINUX)
    ladybird_test(TestLandlock.cpp LibSandbox LIBS LibSandbox)
    ladybird_test(TestSeccomp.cpp LibSandbox LIBS LibSandbox LibCore)

    # Applies the real sandbox of the image decoder, so it builds its sandbox source.
    ladybird_test(TestImageDecoderSandbox.cpp LibSandbox LIBS LibSandbox LibCore)
    target_sources(TestImageDecoderSandbox PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/ImageDecoder/SandboxLinux.cpp)
    target_include_directories(TestImageDecoderSandbox PRIVATE ${LADYBIRD_SOURCE_DIR}/Services)
endif()

if (APPLE)
    ladybird_test(TestSeatbelt.cpp LibSandbox LIBS LibSandbox LibCore)
    target_link_libraries(TestSeatbelt PRIVATE "-framework CoreFoundation" "-framework CoreServices" "-framework IOKit" "-framework IOSurface" "-framework Security")

    # Applies the real sandboxes of the helpers, so it builds their sandbox sources.
    ladybird_test(TestHelperSandboxes.cpp LibSandbox LIBS LibSandbox LibCore LibWebView)
    target_sources(TestHelperSandboxes PRIVATE
        ${LADYBIRD_SOURCE_DIR}/Services/Compositor/SandboxMacOS.cpp
        ${LADYBIRD_SOURCE_DIR}/Services/RendererSandboxMacOS.cpp
    )
    target_include_directories(TestHelperSandboxes PRIVATE ${LADYBIRD_SOURCE_DIR}/Services)
    target_link_libraries(TestHelperSandboxes PRIVATE "-framework CoreFoundation" "-framework CoreGraphics" "-framework Security")
endif()
