// References that frames hold while the collector runs, whichever tier runs them (see gc-frames.wat). Each round
// allocates enough garbage that collections happen in the middle of the functions.

const module = parseWebAssemblyModule(readBinaryWasmFile("Fixtures/Modules/gc-frames.wasm"));
const call = (name, ...args) => module.invoke(module.getExport(name), ...args);
const before = peakResidentMemorySize();

test("a list held by a local survives collections", () => {
    for (let round = 0; round < 3; ++round) expect(call("list", 200)).toBe(19900n);
});

test("references on the operand stack survive collections", () => {
    for (let round = 0; round < 3; ++round) expect(call("operands", 100)).toBe(4950n + 100000n);
});

test("an array of references outside the heap survives collections", () => {
    for (let round = 0; round < 3; ++round) expect(call("array", 1000)).toBe(499500n);
});

test("a large array of numbers survives collections", () => {
    for (let round = 0; round < 3; ++round) expect(call("wide", 1000)).toBe(499500n + 599n);
});

test("vector, float, i31 and cast references survive collections", () => {
    for (let round = 0; round < 3; ++round) expect(call("mixed", 300)).toBe(3n * 44850n + 1800n);
});

test("the garbage was collected", () => {
    // The rounds above allocate and write more than 2 GiB.
    expect(peakResidentMemorySize() - before).toBeLessThan(512 * 1024 * 1024);
});
