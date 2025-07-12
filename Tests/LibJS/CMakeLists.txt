ladybird_test(test-invalid-unicode-js.cpp LibJS LIBS LibJS LibUnicode)
ladybird_test(test-value-js.cpp LibJS LIBS LibJS LibUnicode)

# FIXME: This test is currently not working in the windows-2025 GHA image  due to the Visual Studio version currently being used
if (WIN32 AND ENABLE_WINDOWS_CI)
    return()
endif()

ladybird_testjs_test(test-js.cpp test-js LIBS LibGC)
set_tests_properties(test-js PROPERTIES ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_PROJECT_ROOT})
