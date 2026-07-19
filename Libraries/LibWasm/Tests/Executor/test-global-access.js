function loadGlobalModules() {
    const globalsBinary = readBinaryWasmFile("Fixtures/Modules/cranelift-global-access.wasm");
    const calleeBinary = readBinaryWasmFile("Fixtures/Modules/cranelift-global-cross-module-callee.wasm");
    const callerBinary = readBinaryWasmFile("Fixtures/Modules/cranelift-global-cross-module-caller.wasm");

    const globals = parseWebAssemblyModule(globalsBinary);
    const callee = parseWebAssemblyModule(calleeBinary);
    const caller = parseWebAssemblyModule(callerBinary, { callee });
    return { globals, callee, caller };
}

function exerciseGlobalAccess({ globals, callee, caller }) {
    const readImported = globals.getExport("read_imported");
    const i32Roundtrip = globals.getExport("i32_roundtrip");
    const i64Roundtrip = globals.getExport("i64_roundtrip");
    const f32Roundtrip = globals.getExport("f32_roundtrip");
    const f64Roundtrip = globals.getExport("f64_roundtrip");
    const incrementLoop = globals.getExport("increment_loop");
    const callAndRead = caller.getExport("call_and_read");
    const readCallee = callee.getExport("read");

    expect(globals.invoke(readImported, 0)).toBe(666);
    expect(globals.invoke(i32Roundtrip, 0x12345678)).toBe(0x12345678);
    expect(globals.invoke(i64Roundtrip, 0x123456789abcdefn)).toBe(0x123456789abcdefn);
    expect(globals.invoke(f32Roundtrip, 0x7fc12345)).toBe(0x7fc12345);
    expect(globals.invoke(f64Roundtrip, 0x7ff8123456789abcn)).toBe(0x7ff8123456789abcn);
    expect(globals.invoke(incrementLoop, 25)).toBe(25);
    expect(caller.invoke(callAndRead)).toBe(42);
    expect(callee.invoke(readCallee)).toBe(99);
}

test("global access remains interpreter-compatible", () => {
    const modules = loadGlobalModules();
    exerciseGlobalAccess(modules);

    const growthBinary = readBinaryWasmFile("Fixtures/Modules/global-storage-growth.wasm");
    for (let i = 0; i < 64; ++i) parseWebAssemblyModule(growthBinary);

    exerciseGlobalAccess(modules);
});
