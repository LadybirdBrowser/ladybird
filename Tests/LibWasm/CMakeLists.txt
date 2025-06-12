add_executable(test-wasm test-wasm.cpp)
target_link_libraries(test-wasm AK LibCore LibFileSystem JavaScriptTestRunnerMain LibTest LibWasm LibJS LibCrypto LibGC)
set(wasm_test_root "${SERENITY_PROJECT_ROOT}")
if (INCLUDE_WASM_SPEC_TESTS)
    set(wasm_test_root "${Lagom_BINARY_DIR}")
endif()
add_test(
    NAME Wasm
    COMMAND test-wasm --show-progress=false "${wasm_test_root}/Libraries/LibWasm/Tests"
)
