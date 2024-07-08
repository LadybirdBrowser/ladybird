serenity_testjs_test(test-js.cpp test-js LIBS LibUnicode)

serenity_test(test-invalid-unicode-js.cpp LibJS LIBS LibJS LibUnicode)

serenity_test(test-value-js.cpp LibJS LIBS LibJS LibUnicode)

add_executable(test262-runner test262-runner.cpp)
target_link_libraries(test262-runner PRIVATE LibJS LibCore LibUnicode)
serenity_set_implicit_links(test262-runner)

add_executable(test-test262 test-test262.cpp)
target_link_libraries(test-test262 PRIVATE LibMain LibCore LibFileSystem)
serenity_set_implicit_links(test-test262)
