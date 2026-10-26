const module = parseWebAssemblyModule(readBinaryWasmFile("Fixtures/Modules/struct-supertype-access.wasm"));
const call = (name, ...args) => module.invoke(module.getExport(name), ...args);

test("struct.get through a supertype reads the subtype's data", () => {
    call("allocate");
    for (let round = 0; round < 3; ++round) expect(call("get", 100000)).toBe(1000000n);
});

test("struct.set through a supertype writes the subtype's data", () => {
    call("allocate");
    for (let round = 0; round < 3; ++round)
        expect(call("set", BigInt(round * 100), 100000)).toBe(BigInt(round * 400 + 6));
});
