ladybird_test(test-value-js.cpp LibJS LIBS LibJS LibUnicode)
ladybird_test(test-primitive-string.cpp LibJS LIBS LibJS LibGC)
ladybird_test(test-bytecode-cache.cpp LibJS LIBS LibCrypto LibFileSystem LibGC LibJS)
ladybird_test(test-debugger.cpp LibJS LIBS LibGC LibJS)
ladybird_test(test-native-function-table.cpp LibJS LIBS LibGC LibJS)
ladybird_test(test-type-error-realm.cpp LibJS LIBS LibGC LibJS)

ladybird_testjs_test(test-js.cpp test-js LIBS LibGC)
set_tests_properties(test-js PROPERTIES ENVIRONMENT "LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR}")

if (NOT WIN32)
    add_custom_target(test-js-bytecode ALL DEPENDS test-js "${CMAKE_BINARY_DIR}/bin/test-js-bytecode")
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/bin/test-js-bytecode"
        COMMAND "${CMAKE_COMMAND}" -E copy "${ladybird_SOURCE_DIR}/Tests/LibJS/test-js-bytecode.py" "${CMAKE_BINARY_DIR}/bin/test-js-bytecode"
        DEPENDS "${ladybird_SOURCE_DIR}/Tests/LibJS/test-js-bytecode.py"
    )
    add_test(NAME test-js-bytecode COMMAND "${CMAKE_BINARY_DIR}/bin/test-js-bytecode")
    set_tests_properties(test-js-bytecode PROPERTIES ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})

    add_custom_target(test-js-ast ALL DEPENDS js "${CMAKE_BINARY_DIR}/bin/test-js-ast")
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/bin/test-js-ast"
        COMMAND "${CMAKE_COMMAND}" -E copy "${ladybird_SOURCE_DIR}/Tests/LibJS/test-js-ast.py" "${CMAKE_BINARY_DIR}/bin/test-js-ast"
        DEPENDS "${ladybird_SOURCE_DIR}/Tests/LibJS/test-js-ast.py"
    )
    add_test(NAME test-js-ast COMMAND "${CMAKE_BINARY_DIR}/bin/test-js-ast")
    set_tests_properties(test-js-ast PROPERTIES ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})

    add_custom_target(test-js-debugger ALL DEPENDS js "${CMAKE_BINARY_DIR}/bin/test-js-debugger")
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/bin/test-js-debugger"
        COMMAND "${CMAKE_COMMAND}" -E copy "${ladybird_SOURCE_DIR}/Tests/LibJS/test-js-debugger.py" "${CMAKE_BINARY_DIR}/bin/test-js-debugger"
        DEPENDS "${ladybird_SOURCE_DIR}/Tests/LibJS/test-js-debugger.py"
    )
    add_test(NAME test-js-debugger COMMAND "${CMAKE_BINARY_DIR}/bin/test-js-debugger")
    set_tests_properties(test-js-debugger PROPERTIES ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})
endif()
