test("memfill executes and returns expected result", () => {
    const bin = readBinaryWasmFile("Fixtures/Modules/memory_fill-order.wasm");
    const module = parseWebAssemblyModule(bin);

    const test_fill = module.getExport("test_fill");
    // memory.fill off=10 value=42 size=5
    module.invoke(test_fill);

    const get_value = module.getExport("get_value");

    // After memory.fill, bytes 10..14 should be filled with value 42
    expect(module.invoke(get_value, 10)).toBe(42);
    expect(module.invoke(get_value, 11)).toBe(42);
    expect(module.invoke(get_value, 12)).toBe(42);
    expect(module.invoke(get_value, 13)).toBe(42);
    expect(module.invoke(get_value, 14)).toBe(42);

    // Byte 9 (before the fill region) should still be 0
    expect(module.invoke(get_value, 9)).toBe(0);

    // Byte 15 (after the fill region) should still be 0
    expect(module.invoke(get_value, 15)).toBe(0);
});
