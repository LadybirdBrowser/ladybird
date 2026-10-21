test("objects, arrays, and exceptions that become garbage are collected", () => {
    const module = parseWebAssemblyModule(readBinaryWasmFile("Fixtures/Modules/gc-churn.wasm"));
    const churn = module.getExport("churn");
    const before = peakResidentMemorySize();
    // 100000 iterations allocate more than 6 GiB of arrays alone.
    expect(module.invoke(churn, 100000)).toBe(99999n);
    const growth = peakResidentMemorySize() - before;
    expect(growth).toBeLessThan(1024 * 1024 * 1024);
});
