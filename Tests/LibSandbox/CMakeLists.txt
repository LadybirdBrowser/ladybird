if (LINUX)
    ladybird_test(TestSeccomp.cpp LibSandbox LIBS LibSandbox LibCore)
endif()

if (APPLE)
    ladybird_test(TestSeatbelt.cpp LibSandbox LIBS LibSandbox LibCore)
    target_link_libraries(TestSeatbelt PRIVATE "-framework CoreFoundation" "-framework CoreServices" "-framework IOKit")
endif()
