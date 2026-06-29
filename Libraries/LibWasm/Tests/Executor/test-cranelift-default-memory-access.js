test("compiled default memory loads and stores use checked caged addresses", () => {
    // prettier-ignore
    const binary = new Uint8Array([
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x06, 0x01, 0x60,
        0x01, 0x7f, 0x01, 0x7f, 0x03, 0x05, 0x04, 0x00, 0x00, 0x00, 0x00, 0x05,
        0x03, 0x01, 0x00, 0x01, 0x07, 0x34, 0x04, 0x09, 0x72, 0x6f, 0x75, 0x6e,
        0x64, 0x74, 0x72, 0x69, 0x70, 0x00, 0x00, 0x07, 0x6c, 0x6f, 0x61, 0x64,
        0x5f, 0x61, 0x74, 0x00, 0x01, 0x08, 0x73, 0x74, 0x6f, 0x72, 0x65, 0x5f,
        0x61, 0x74, 0x00, 0x02, 0x0f, 0x6c, 0x6f, 0x61, 0x64, 0x5f, 0x6d, 0x61,
        0x78, 0x5f, 0x6f, 0x66, 0x66, 0x73, 0x65, 0x74, 0x00, 0x03, 0x0a, 0x30,
        0x04, 0x0e, 0x00, 0x41, 0x20, 0x20, 0x00, 0x36, 0x02, 0x00, 0x41, 0x20,
        0x28, 0x02, 0x00, 0x0b, 0x07, 0x00, 0x20, 0x00, 0x28, 0x02, 0x00, 0x0b,
        0x0b, 0x00, 0x20, 0x00, 0x41, 0x2a, 0x36, 0x02, 0x00, 0x41, 0x01, 0x0b,
        0x0b, 0x00, 0x20, 0x00, 0x28, 0x02, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x0b
    ]);

    const module = parseWebAssemblyModule(binary);
    const roundtrip = module.getExport("roundtrip");
    const loadAt = module.getExport("load_at");
    const storeAt = module.getExport("store_at");
    const loadMaxOffset = module.getExport("load_max_offset");

    const roundtripEligible = isCraneliftEligible(roundtrip);
    const loadAtEligible = isCraneliftEligible(loadAt);
    const storeAtEligible = isCraneliftEligible(storeAt);
    const loadMaxOffsetEligible = isCraneliftEligible(loadMaxOffset);

    if (!roundtripEligible && !loadAtEligible && !storeAtEligible && !loadMaxOffsetEligible) return;

    expect(roundtripEligible).toBe(true);
    expect(loadAtEligible).toBe(true);
    expect(storeAtEligible).toBe(true);
    expect(loadMaxOffsetEligible).toBe(true);

    const roundtripCompiled = isCraneliftCompiled(roundtrip);
    const loadAtCompiled = isCraneliftCompiled(loadAt);
    const storeAtCompiled = isCraneliftCompiled(storeAt);
    const loadMaxOffsetCompiled = isCraneliftCompiled(loadMaxOffset);

    expect(roundtripCompiled).toBe(true);
    expect(loadAtCompiled).toBe(true);
    expect(storeAtCompiled).toBe(true);
    expect(loadMaxOffsetCompiled).toBe(true);
    expect(module.invoke(roundtrip, 0x12345678)).toBe(0x12345678);
    expect(() => module.invoke(loadAt, 65534)).toThrowWithMessage(
        TypeError,
        "Execution trapped: Memory access out of bounds"
    );
    expect(() => module.invoke(storeAt, 65534)).toThrowWithMessage(
        TypeError,
        "Execution trapped: Memory access out of bounds"
    );
    expect(() => module.invoke(loadMaxOffset, 1)).toThrowWithMessage(
        TypeError,
        "Execution trapped: Memory access out of bounds"
    );
});
