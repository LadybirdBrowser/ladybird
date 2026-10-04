if (LIBJS_RUNTIME STREQUAL "Cpp")
    ladybird_test(test-value-js.cpp LibJS LIBS LibGC LibJS LibUnicode)
    ladybird_test(test-primitive-string.cpp LibJS LIBS LibJS LibGC)
    ladybird_test(test-bytecode-cache.cpp LibJS LIBS LibCrypto LibFileSystem LibGC LibJS)
    ladybird_test(test-slow-path-result.cpp LibJS LIBS LibJS)
    ladybird_test(test-debugger.cpp LibJS LIBS LibGC LibJS)
    ladybird_test(test-native-function-table.cpp LibJS LIBS LibGC LibJS)
    ladybird_test(test-type-error-realm.cpp LibJS LIBS LibGC LibJS)
    ladybird_test(test-console.cpp LibJS LIBS LibGC LibJS)
    ladybird_test(test-host-object.cpp LibJS LIBS LibGC LibJS)

    ladybird_testjs_test(test-js.cpp test-js NAME test-js-runtime NO_TEST LIBS LibGC)
endif()

ladybird_utility(test-js SOURCES test-js-main.cpp LIBS LibMain)
ladybird_windows_bin(test-js CONSOLE)
target_compile_definitions(test-js PRIVATE TEST_JS_PYTHON="${Python3_EXECUTABLE}")

add_custom_target(test-js-script DEPENDS "${CMAKE_BINARY_DIR}/bin/test-js.py")
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/bin/test-js.py"
    COMMAND "${CMAKE_COMMAND}" -E copy "${ladybird_SOURCE_DIR}/Tests/LibJS/test-js.py" "${CMAKE_BINARY_DIR}/bin/test-js.py"
    DEPENDS "${ladybird_SOURCE_DIR}/Tests/LibJS/test-js.py"
)
if (LIBJS_RUNTIME STREQUAL "Rust")
    add_dependencies(test-js test-js-runtime-rust test-js-script js-rust)
    add_test(NAME test-js COMMAND test-js --js $<TARGET_FILE:js-rust> --runtime $<TARGET_FILE:test-js-runtime-rust>)
else()
    add_dependencies(test-js test-js-runtime test-js-script js)
    add_test(NAME test-js COMMAND test-js)
endif()
set_tests_properties(test-js PROPERTIES ENVIRONMENT "LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR}")

if (ENABLE_LIBJS_RUST_RUNTIME)
    ladybird_utility(test-js-runtime-rust SOURCES test-js-runtime-rust.cpp LIBS libjs_runtime_rust_tool_entry_points LibMain)
    add_test(NAME js-rust-ast COMMAND test-js --suite ast --js $<TARGET_FILE:js-rust>)
    add_test(NAME js-rust-bytecode COMMAND test-js --suite bytecode --js $<TARGET_FILE:js-rust>)
    add_test(NAME js-rust-runtime COMMAND test-js --suite runtime --runtime $<TARGET_FILE:test-js-runtime-rust>)
    set_tests_properties(js-rust-ast js-rust-bytecode js-rust-runtime PROPERTIES ENVIRONMENT "LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR}")
endif()
