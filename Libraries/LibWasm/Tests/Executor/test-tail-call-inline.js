test("a callee containing return_call is not inlined", () => {
    const bin = readBinaryWasmFile("Fixtures/Modules/tail-call-inline.wasm");
    const module = parseWebAssemblyModule(bin);

    const run = module.getExport("run");
    expect(module.invoke(run)).toBe(8);
});
