// Mutual tail calls between functions with different numbers of stack arguments run in constant stack space: a
// million of them would exhaust the stack otherwise.
test("mutual tail calls of different arities do not grow the stack", () => {
    const bin = readBinaryWasmFile("Fixtures/Modules/tail-call-mutual-arities.wasm");
    const module = parseWebAssemblyModule(bin);

    const few = module.getExport("few");
    expect(module.invoke(few, 5)).toBe(0);
    expect(module.invoke(few, 1000000)).toBe(0);
});

test("mutual tail calls with a result area do not grow the stack", () => {
    const bin = readBinaryWasmFile("Fixtures/Modules/tail-call-mutual-arities.wasm");
    const module = parseWebAssemblyModule(bin);

    const fewResults = module.getExport("few_results");
    expect(module.invoke(fewResults, 1000000)).toEqual([1, 2, 3]);
});
