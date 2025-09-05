test("memfill executes and returns expected result", () => {
    const bin = readBinaryWasmFile("Fixtures/Modules/memfill-memidx.wasm");

    const module = parseWebAssemblyModule(bin);

    const go = module.getExport("go");
    const result = module.invoke(go);

    // mem1[0]=0xAA, mem0[0]=0x00 → 0xAA00 = 43520
    expect(result).toBe(43520);
});
