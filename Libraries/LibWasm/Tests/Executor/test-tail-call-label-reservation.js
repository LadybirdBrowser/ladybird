test("a tail call reserves the callee's labels", () => {
    // A tail call replaces the current frame, but the callee ($deep, 64 nested blocks) can nest deeper than
    // whatever label capacity the replaced frame left behind. The callee pushes its labels unchecked, so
    // without reserving for the callee here this overflows the inline label storage and corrupts the frame
    // stack. Just needs to run to completion without crashing.
    const bin = readBinaryWasmFile("Fixtures/Modules/tail-call-label-reservation.wasm");
    const module = parseWebAssemblyModule(bin);

    const run = module.getExport("run");
    module.invoke(run, 0);
});
