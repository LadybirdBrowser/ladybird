test("blocks nested across calls", () => {
    // At the deepest point, the blocks of $caller and of $inlinee, which it calls, are open together.
    const bin = readBinaryWasmFile("Fixtures/Modules/inline-label-reservation.wasm");
    const module = parseWebAssemblyModule(bin);

    const run = module.getExport("run");
    const result = module.invoke(run, 0);
    expect(result[0]).toBe(0);
    expect(result[1]).toBe(0);
});
