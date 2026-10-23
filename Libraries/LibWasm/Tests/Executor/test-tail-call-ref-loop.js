// The callee's instance of a `return_call_ref` in a loop that makes no other call is read behind the reference's
// null check, so the loop runs, and calling the null reference traps, in whichever tier runs it (see
// tail-call-ref-loop.wat). The long loops are there to have the function compiled when the JIT tiers up.
test("a loop that does not take its return_call_ref does not read the callee", () => {
    const module = parseWebAssemblyModule(readBinaryWasmFile("Fixtures/Modules/tail-call-ref-loop.wasm"));
    const run = module.getExport("run");

    for (let round = 0; round < 3; ++round) expect(module.invoke(run, 0, 100000)).toBe(7);
    expect(() => module.invoke(run, 1, 100000)).toThrowWithMessage(TypeError, "Execution trapped: null reference");
});
