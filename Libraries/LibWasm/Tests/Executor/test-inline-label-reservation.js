test("inlining recomputes the frame's label reservation", () => {
    // $inlinee is spliced into $caller, so at the deepest point the label stack holds the caller's blocks
    // plus a wrapper block plus the inlinee's blocks — far more than the validator counted for $caller alone.
    // Labels are pushed unchecked, so without recomputing the reservation post-inlining this overflows the
    // frame's inline label storage and corrupts the adjacent frame stack. $caller returns two values to stay
    // on the interpreter path (and off Cranelift), which is what actually pushes the labels.
    const bin = readBinaryWasmFile("Fixtures/Modules/inline-label-reservation.wasm");
    const module = parseWebAssemblyModule(bin);

    const run = module.getExport("run");
    const result = module.invoke(run, 0);
    expect(result[0]).toBe(0);
    expect(result[1]).toBe(0);
});
