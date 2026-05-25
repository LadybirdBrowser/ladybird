test("return mid-function doesn't corrupt caller stack", () => {
    const bin = readBinaryWasmFile("Fixtures/Modules/return-mid-function.wasm");
    const module = parseWebAssemblyModule(bin);

    // Deterministic check: without the fix, the residual 99 from $leaky sits between the caller’s 10 and the result 42,
    // so i32.add yields 141, rather than the correct 52.
    const test_add = module.getExport("test_add");
    expect(module.invoke(test_add)).toBe(52);

    // Stress check: 100 iterations accumulate residuals under the bug; under ASan this overflows the value stack’s
    // inline storage.
    const drive = module.getExport("drive");
    expect(module.invoke(drive)).toBe(100 * 42);
});
