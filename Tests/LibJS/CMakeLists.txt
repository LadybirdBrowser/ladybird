ladybird_utility(test-js-runtime SOURCES test-js-runtime.cpp LIBS LibJSStandalone LibMain)

ladybird_test(test-debugger.cpp LibJS LIBS LibJSStandalone)

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

# The JIT's tests: the runtime tests (each run three times, so that the functions they call get compiled with
# feedback) and the programs in Tests/LibJS/JIT/programs, under the fast tier of JIT stress modes and compared with the
# interpreter; and the checks of the IR the JIT's passes produce. Tests/LibJS/JIT/matrix.py runs more modes and suites.
if (ENABLE_JS_JIT)
    cmake_host_system_information(RESULT logical_core_count QUERY NUMBER_OF_LOGICAL_CORES)
    add_test(
        NAME test-js-jit
        COMMAND "${Python3_EXECUTABLE}" "${ladybird_SOURCE_DIR}/Tests/LibJS/JIT/matrix.py" --build "${CMAKE_BINARY_DIR}"
            --modes fast --suites test-js,programs --results "${CMAKE_BINARY_DIR}/Testing/jit-matrix"
            --jobs ${logical_core_count}
    )
    add_test(
        NAME test-jit-ir
        COMMAND "${Python3_EXECUTABLE}" "${ladybird_SOURCE_DIR}/Tests/LibJS/JIT/irtest.py" --build "${CMAKE_BINARY_DIR}"
    )
endif()

add_custom_target(test-js-debugger ALL DEPENDS js "${CMAKE_BINARY_DIR}/bin/test-js-debugger")
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/bin/test-js-debugger"
    COMMAND "${CMAKE_COMMAND}" -E copy "${ladybird_SOURCE_DIR}/Tests/LibJS/test-js-debugger.py" "${CMAKE_BINARY_DIR}/bin/test-js-debugger"
    DEPENDS "${ladybird_SOURCE_DIR}/Tests/LibJS/test-js-debugger.py"
)
add_test(NAME test-js-debugger COMMAND "${Python3_EXECUTABLE}" "${CMAKE_BINARY_DIR}/bin/test-js-debugger")
set_tests_properties(test-js-debugger PROPERTIES ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})
