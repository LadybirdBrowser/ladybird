const module = parseWebAssemblyModule(readBinaryWasmFile("Fixtures/Modules/exception-caught-by-caller.wasm"));
const call = (name, ...args) => module.invoke(module.getExport(name), ...args);
const values = Array.from({ length: 16 }, (_, index) => index);

test("a handler in the caller receives the values its callee throws", () => {
    expect(call("catch_from_callee")).toEqual(values);
});

test("a handler receives the values a compiled function throws to the caller of its caller", () => {
    call("compile_thrower", 10000);
    expect(call("catch_from_compiled_callee")).toEqual(values);
});

test("a handler in the caller receives the values its callee throws once it continues compiled at a loop", () => {
    expect(call("catch_from_callee_continued_compiled")).toEqual(values);
});
