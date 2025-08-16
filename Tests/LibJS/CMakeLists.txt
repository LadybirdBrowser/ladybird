ladybird_test(test-invalid-unicode-js.cpp LibJS LIBS LibJS LibUnicode)
ladybird_test(test-value-js.cpp LibJS LIBS LibJS LibUnicode)

ladybird_testjs_test(test-js.cpp test-js LIBS LibGC)
set_tests_properties(test-js PROPERTIES ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_PROJECT_ROOT})
