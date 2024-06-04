serenity_testjs_test(test-js.cpp test-js LIBS LibLocale)

install(TARGETS test-js RUNTIME DESTINATION bin OPTIONAL)

serenity_test(test-invalid-unicode-js.cpp LibJS LIBS LibJS LibLocale)

serenity_test(test-value-js.cpp LibJS LIBS LibJS LibLocale)

add_executable(test262-runner test262-runner.cpp)
target_link_libraries(test262-runner PRIVATE LibJS LibCore LibLocale)
serenity_set_implicit_links(test262-runner)
install(TARGETS test262-runner RUNTIME DESTINATION bin OPTIONAL)

add_executable(test-test262 test-test262.cpp)
target_link_libraries(test-test262 PRIVATE LibMain LibCore LibFileSystem)
serenity_set_implicit_links(test-test262)
install(TARGETS test-test262 RUNTIME DESTINATION bin OPTIONAL)
