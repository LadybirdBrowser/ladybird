ladybird_utility(test-js-runtime SOURCES test-js-runtime.cpp LIBS LibJS LibMain)

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

# The runtime tests again, with the interpreter's profiling tier on, which the tests in jit directories need.
if (ENABLE_JS_JIT)
    add_test(NAME test-js-profiling COMMAND test-js --suite runtime)
    set_tests_properties(test-js-profiling PROPERTIES ENVIRONMENT "LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR};LIBJS_JIT=on")
endif()
