test("cross-module zero-local wasm call restores caller memory", () => {
    const calleeBin = readBinaryWasmFile("Fixtures/Modules/zero-local-cross-module-memory-callee.wasm");
    const callerBin = readBinaryWasmFile("Fixtures/Modules/zero-local-cross-module-memory-caller.wasm");

    const callee = parseWebAssemblyModule(calleeBin);
    const caller = parseWebAssemblyModule(callerBin, { callee });

    const fn = caller.getExport("call_and_read");
    expect(caller.invoke(fn)).toBe(42);
});
