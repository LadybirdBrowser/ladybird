ladybird_test(test-value-js.cpp LibJS LIBS LibJS LibUnicode)
ladybird_test(test-primitive-string.cpp LibJS LIBS LibJS LibGC)
ladybird_test(test-bytecode-cache.cpp LibJS LIBS LibCrypto LibFileSystem LibGC LibJS)
ladybird_test(test-slow-path-result.cpp LibJS LIBS LibJS)
ladybird_test(test-debugger.cpp LibJS LIBS LibGC LibJS)
ladybird_test(test-native-function-table.cpp LibJS LIBS LibGC LibJS)
ladybird_test(test-type-error-realm.cpp LibJS LIBS LibGC LibJS)

ladybird_testjs_test(test-js.cpp test-js NAME test-js-runtime NO_TEST LIBS LibGC)
ladybird_utility(test-js SOURCES test-js-main.cpp LIBS LibMain)
ladybird_windows_bin(test-js CONSOLE)
target_compile_definitions(test-js PRIVATE TEST_JS_PYTHON="${Python3_EXECUTABLE}")

add_custom_target(test-js-script DEPENDS "${CMAKE_BINARY_DIR}/bin/test-js.py")
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/bin/test-js.py"
    COMMAND "${CMAKE_COMMAND}" -E copy "${ladybird_SOURCE_DIR}/Tests/LibJS/test-js.py" "${CMAKE_BINARY_DIR}/bin/test-js.py"
    DEPENDS "${ladybird_SOURCE_DIR}/Tests/LibJS/test-js.py"
)
add_dependencies(test-js test-js-runtime test-js-script js)
add_test(NAME test-js COMMAND test-js)
set_tests_properties(test-js PROPERTIES ENVIRONMENT "LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR}")
