const module = parseWebAssemblyModule(readBinaryWasmFile("Fixtures/Modules/frame-results-reservation.wasm"));
const call = (name, ...args) => module.invoke(module.getExport(name), ...args);
const values = Array.from({ length: 64 }, (_, index) => index);

test("an exception caught at the function's own label returns its values", () => {
    expect(call("catch_values")).toEqual(values);
});

test("a function continued compiled at a loop returns the results of its tail call", () => {
    call("call_values", 10000);
    expect(call("loop_then_tail_call_values")).toEqual(values);
});

test("a function called from the host returns its results once it runs compiled", () => {
    for (let round = 0; round < 1000; ++round) expect(call("values")).toEqual(values);
});
