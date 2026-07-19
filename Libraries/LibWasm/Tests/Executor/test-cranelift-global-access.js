test("compiled global reads and writes directly access stable instances", () => {
    const globalsBinary = readBinaryWasmFile("Fixtures/Modules/cranelift-global-access.wasm");
    const calleeBinary = readBinaryWasmFile("Fixtures/Modules/cranelift-global-cross-module-callee.wasm");
    const callerBinary = readBinaryWasmFile("Fixtures/Modules/cranelift-global-cross-module-caller.wasm");

    const globals = parseWebAssemblyModule(globalsBinary);
    const callee = parseWebAssemblyModule(calleeBinary);
    const caller = parseWebAssemblyModule(callerBinary, { callee });
    const functions = [
        ["read_imported", globals, globals.getExport("read_imported")],
        ["i32_roundtrip", globals, globals.getExport("i32_roundtrip")],
        ["i64_roundtrip", globals, globals.getExport("i64_roundtrip")],
        ["f32_roundtrip", globals, globals.getExport("f32_roundtrip")],
        ["f64_roundtrip", globals, globals.getExport("f64_roundtrip")],
        ["increment_loop", globals, globals.getExport("increment_loop")],
        ["touch", callee, callee.getExport("touch")],
        ["read", callee, callee.getExport("read")],
        ["call_and_read", caller, caller.getExport("call_and_read")],
    ];

    if (functions.every(([, , fn]) => !isCraneliftEligible(fn))) return;

    for (const [name, , fn] of functions) {
        if (!isCraneliftEligible(fn)) throw new Error(`${name} is not Cranelift-eligible`);
        if (!isCraneliftCompiled(fn)) throw new Error(`${name} did not compile with Cranelift`);
    }

    expect(globals.invoke(functions[0][2], 0)).toBe(666);
    expect(globals.invoke(functions[1][2], 0x12345678)).toBe(0x12345678);
    expect(globals.invoke(functions[2][2], 0x123456789abcdefn)).toBe(0x123456789abcdefn);
    expect(globals.invoke(functions[3][2], 0x7fc12345)).toBe(0x7fc12345);
    expect(globals.invoke(functions[4][2], 0x7ff8123456789abcn)).toBe(0x7ff8123456789abcn);
    expect(globals.invoke(functions[5][2], 25)).toBe(25);
    expect(caller.invoke(functions[8][2])).toBe(42);
    expect(callee.invoke(functions[7][2])).toBe(99);

    const growthBinary = readBinaryWasmFile("Fixtures/Modules/global-storage-growth.wasm");
    for (let i = 0; i < 64; ++i) parseWebAssemblyModule(growthBinary);

    expect(globals.invoke(functions[1][2], 0x76543210)).toBe(0x76543210);
    expect(caller.invoke(functions[8][2])).toBe(42);
});
