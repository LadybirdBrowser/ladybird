test("compiled loads and stores directly access every memory instance", () => {
    const binary = readBinaryWasmFile("Fixtures/Modules/cranelift-multi-memory-access.wasm");
    const module = parseWebAssemblyModule(binary);
    const roundtrip = module.getExport("roundtrip");
    const loadAt = module.getExport("load_at");
    const storeAt = module.getExport("store_at");
    const memoryOneIsDistinct = module.getExport("memory_one_is_distinct");
    const growAndRoundtrip = module.getExport("grow_and_roundtrip");
    const callGrowAndRoundtrip = module.getExport("call_grow_and_roundtrip");
    const functions = [roundtrip, loadAt, storeAt, memoryOneIsDistinct, growAndRoundtrip, callGrowAndRoundtrip];

    if (functions.every(fn => !isCraneliftEligible(fn))) return;

    for (const fn of functions) {
        expect(isCraneliftEligible(fn)).toBe(true);
        expect(isCraneliftCompiled(fn)).toBe(true);
    }

    expect(module.invoke(roundtrip, 0x12345678)).toBe(0x12345678);
    expect(module.invoke(memoryOneIsDistinct)).toBe(0);
    expect(() => module.invoke(loadAt, 65534)).toThrowWithMessage(
        TypeError,
        "Execution trapped: Memory access out of bounds"
    );
    expect(() => module.invoke(storeAt, 65534)).toThrowWithMessage(
        TypeError,
        "Execution trapped: Memory access out of bounds"
    );
    expect(module.invoke(growAndRoundtrip, 0x24681357)).toBe(0x24681357);
    expect(module.invoke(callGrowAndRoundtrip, 0x76543210)).toBe(0x76543210);
});
