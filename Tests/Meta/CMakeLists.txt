if (NOT WIN32)
    add_custom_target(test-css-grammar-parser ALL DEPENDS "${CMAKE_BINARY_DIR}/bin/test-css-grammar-parser.py")
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/bin/test-css-grammar-parser.py"
        COMMAND "${CMAKE_COMMAND}" -E copy "${ladybird_SOURCE_DIR}/Tests/Meta/test-css-grammar-parser.py" "${CMAKE_BINARY_DIR}/bin/test-css-grammar-parser.py"
        DEPENDS "${ladybird_SOURCE_DIR}/Tests/Meta/test-css-grammar-parser.py"
    )
    add_test(NAME test-css-grammar-parser COMMAND "${Python3_EXECUTABLE}" "${CMAKE_BINARY_DIR}/bin/test-css-grammar-parser.py")
    set_tests_properties(test-css-grammar-parser PROPERTIES ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})
endif()
