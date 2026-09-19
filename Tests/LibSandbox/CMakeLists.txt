if (LINUX)
    ladybird_test(TestLandlock.cpp LibSandbox LIBS LibSandbox)
    ladybird_test(TestSeccomp.cpp LibSandbox LIBS LibSandbox LibCore)
endif()

if (APPLE)
    ladybird_test(TestSeatbelt.cpp LibSandbox LIBS LibSandbox LibCore)
    target_link_libraries(TestSeatbelt PRIVATE "-framework CoreFoundation" "-framework CoreServices" "-framework IOKit" "-framework IOSurface")

    # Applies the real sandboxes of the helpers, so it builds their sandbox sources.
    ladybird_test(TestHelperSandboxes.cpp LibSandbox LIBS LibSandbox LibCore LibWebView)
    target_sources(TestHelperSandboxes PRIVATE
        ${LADYBIRD_SOURCE_DIR}/Services/Compositor/SandboxMacOS.cpp
        ${LADYBIRD_SOURCE_DIR}/Services/RendererSandboxMacOS.cpp
    )
    target_include_directories(TestHelperSandboxes PRIVATE ${LADYBIRD_SOURCE_DIR}/Services)
    target_link_libraries(TestHelperSandboxes PRIVATE "-framework CoreFoundation" "-framework Security")
endif()
