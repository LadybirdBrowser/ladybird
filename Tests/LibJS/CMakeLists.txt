ladybird_test(test-value-js.cpp LibJS LIBS LibJS LibUnicode)

ladybird_testjs_test(test-js.cpp test-js LIBS LibGC)
set_tests_properties(test-js PROPERTIES ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_PROJECT_ROOT})

if (NOT WIN32)
    add_custom_target(test-js-bytecode ALL DEPENDS test-js "${CMAKE_BINARY_DIR}/bin/test-js-bytecode")
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/bin/test-js-bytecode"
        COMMAND "${CMAKE_COMMAND}" -E copy "${ladybird_SOURCE_DIR}/Tests/LibJS/test-js-bytecode.py" "${CMAKE_BINARY_DIR}/bin/test-js-bytecode"
        DEPENDS "${ladybird_SOURCE_DIR}/Tests/LibJS/test-js-bytecode.py"
    )
    add_test(NAME test-js-bytecode COMMAND "${CMAKE_BINARY_DIR}/bin/test-js-bytecode")
    set_tests_properties(test-js-bytecode PROPERTIES ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_PROJECT_ROOT})

    add_custom_target(test-js-ast ALL DEPENDS js "${CMAKE_BINARY_DIR}/bin/test-js-ast")
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/bin/test-js-ast"
        COMMAND "${CMAKE_COMMAND}" -E copy "${ladybird_SOURCE_DIR}/Tests/LibJS/test-js-ast.py" "${CMAKE_BINARY_DIR}/bin/test-js-ast"
        DEPENDS "${ladybird_SOURCE_DIR}/Tests/LibJS/test-js-ast.py"
    )
    add_test(NAME test-js-ast COMMAND "${CMAKE_BINARY_DIR}/bin/test-js-ast")
    set_tests_properties(test-js-ast PROPERTIES ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_PROJECT_ROOT})
endif()
