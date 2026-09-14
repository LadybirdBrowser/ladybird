// Threads-proposal atomics on non-shared memory: they behave like plain accesses, except that
// they trap when misaligned, waits always trap, and notifies never have anyone to wake.

function loadAtomics() {
    const bin = readBinaryWasmFile("Fixtures/Modules/atomics.wasm");
    const module = parseWebAssemblyModule(bin);
    const exports = {};
    for (const name of [
        "i32_load",
        "i32_store",
        "i32_load8_u",
        "i32_store8",
        "i32_load16_u",
        "i64_load",
        "i64_store",
        "i64_load32_u",
        "i64_store16",
        "i32_rmw_add",
        "i32_rmw_sub",
        "i32_rmw_and",
        "i32_rmw_or",
        "i32_rmw_xor",
        "i32_rmw_xchg",
        "i32_rmw8_add_u",
        "i32_rmw16_sub_u",
        "i64_rmw_add",
        "i64_rmw16_xchg_u",
        "i64_rmw32_or_u",
        "i32_cmpxchg",
        "i32_cmpxchg8_u",
        "i64_cmpxchg",
        "i64_cmpxchg32_u",
        "notify",
        "wait32",
        "wait64",
        "fence",
        "i32_load_offset",
        "plain_i32_load",
    ]) {
        const fn = module.getExport(name);
        exports[name] = (...args) => module.invoke(fn, ...args);
    }
    return exports;
}

test("atomic loads and stores behave like plain ones", () => {
    const m = loadAtomics();

    m.i32_store(8, 0x12345678);
    expect(m.i32_load(8)).toBe(0x12345678);
    expect(m.plain_i32_load(8)).toBe(0x12345678);
    expect(m.i32_load_offset(4)).toBe(0x12345678);
    expect(m.i32_load8_u(8)).toBe(0x78);
    expect(m.i32_load8_u(11)).toBe(0x12);
    expect(m.i32_load16_u(10)).toBe(0x1234);

    m.i32_store8(16, 0x1ff);
    expect(m.i32_load(16)).toBe(0xff);

    m.i64_store(24, 0x1122334455667788n);
    expect(m.i64_load(24)).toBe(0x1122334455667788n);
    expect(m.i64_load32_u(24)).toBe(0x55667788n);
    expect(m.i64_load32_u(28)).toBe(0x11223344n);

    m.i64_store16(32, 0x1abcdn);
    expect(m.i64_load(32)).toBe(0xabcdn);
});

test("read-modify-write returns the old value and updates memory", () => {
    const m = loadAtomics();

    m.i32_store(0, 10);
    expect(m.i32_rmw_add(0, 5)).toBe(10);
    expect(m.i32_load(0)).toBe(15);
    expect(m.i32_rmw_sub(0, 3)).toBe(15);
    expect(m.i32_load(0)).toBe(12);
    expect(m.i32_rmw_and(0, 0xa)).toBe(12);
    expect(m.i32_load(0)).toBe(8);
    expect(m.i32_rmw_or(0, 3)).toBe(8);
    expect(m.i32_load(0)).toBe(11);
    expect(m.i32_rmw_xor(0, 1)).toBe(11);
    expect(m.i32_load(0)).toBe(10);
    expect(m.i32_rmw_xchg(0, 99)).toBe(10);
    expect(m.i32_load(0)).toBe(99);

    // Narrow operations wrap at the access width and zero-extend their result.
    m.i32_store(4, 0);
    expect(m.i32_rmw8_add_u(4, 0x1ff)).toBe(0);
    expect(m.i32_load(4)).toBe(0xff);
    expect(m.i32_rmw8_add_u(4, 1)).toBe(0xff);
    expect(m.i32_load(4)).toBe(0);
    m.i32_store(4, 0x00010000);
    expect(m.i32_rmw16_sub_u(4, 1)).toBe(0);
    expect(m.i32_load(4)).toBe(0x0001ffff);

    m.i64_store(8, 5n);
    expect(m.i64_rmw_add(8, 0xffffffffn)).toBe(5n);
    expect(m.i64_load(8)).toBe(0x100000004n);
    expect(m.i64_rmw16_xchg_u(8, 0x1abcdn)).toBe(4n);
    expect(m.i64_load(8)).toBe(0x10000abcdn);
    expect(m.i64_rmw32_or_u(12, 0x100000001n)).toBe(1n);
    expect(m.i64_load(8)).toBe(0x10000abcdn);
});

test("cmpxchg compares at the access width", () => {
    const m = loadAtomics();

    m.i32_store(0, 7);
    expect(m.i32_cmpxchg(0, 8, 100)).toBe(7);
    expect(m.i32_load(0)).toBe(7);
    expect(m.i32_cmpxchg(0, 7, 100)).toBe(7);
    expect(m.i32_load(0)).toBe(100);

    m.i32_store(0, 0x1234);
    expect(m.i32_cmpxchg8_u(0, 0x134, 0x99)).toBe(0x34);
    expect(m.i32_load(0)).toBe(0x1299);
    expect(m.i32_cmpxchg8_u(1, 0x99, 0x77)).toBe(0x12);
    expect(m.i32_load(0)).toBe(0x1299);

    m.i64_store(8, 0xffffffffffffffffn);
    expect(m.i64_cmpxchg(8, 0xffffffffffffffffn, 1n)).toBe(-1n);
    expect(m.i64_load(8)).toBe(1n);
    expect(m.i64_cmpxchg32_u(8, 0x100000001n, 2n)).toBe(1n);
    expect(m.i64_load(8)).toBe(2n);
    expect(m.i64_cmpxchg32_u(12, 1n, 3n)).toBe(0n);
    expect(m.i64_load(8)).toBe(2n);
});

test("atomic accesses trap on misaligned or out-of-bounds addresses", () => {
    const m = loadAtomics();
    const unaligned = "Execution trapped: Unaligned atomic memory access";
    const outOfBounds = "Execution trapped: Memory access out of bounds";

    expect(() => m.i32_load(2)).toThrowWithMessage(TypeError, unaligned);
    expect(() => m.i32_store(1, 0)).toThrowWithMessage(TypeError, unaligned);
    expect(() => m.i64_load(4)).toThrowWithMessage(TypeError, unaligned);
    expect(() => m.i64_load32_u(2)).toThrowWithMessage(TypeError, unaligned);
    expect(() => m.i32_rmw_add(2, 1)).toThrowWithMessage(TypeError, unaligned);
    expect(() => m.i32_cmpxchg(1, 0, 0)).toThrowWithMessage(TypeError, unaligned);
    expect(() => m.i32_load_offset(2)).toThrowWithMessage(TypeError, unaligned);
    expect(m.i32_load8_u(3)).toBe(0);
    expect(m.i32_load16_u(6)).toBe(0);

    expect(() => m.i32_load(65536)).toThrowWithMessage(TypeError, outOfBounds);
    expect(() => m.i32_load(65534)).toThrowWithMessage(TypeError, outOfBounds);
    expect(() => m.i32_load(-4)).toThrowWithMessage(TypeError, outOfBounds);
    expect(() => m.i64_store(65532, 0n)).toThrowWithMessage(TypeError, outOfBounds);
    expect(() => m.i32_rmw_add(65536, 1)).toThrowWithMessage(TypeError, outOfBounds);
    expect(() => m.i32_load_offset(65532)).toThrowWithMessage(TypeError, outOfBounds);
    expect(m.i32_load(65532)).toBe(0);
});

test("waits trap and notifies wake nobody on non-shared memory", () => {
    const m = loadAtomics();
    const unaligned = "Execution trapped: Unaligned atomic memory access";
    const outOfBounds = "Execution trapped: Memory access out of bounds";
    const notShared = "Execution trapped: Expected shared memory";

    expect(m.notify(0, 1)).toBe(0);
    expect(m.notify(4, 0xffffffff)).toBe(0);
    expect(() => m.notify(2, 1)).toThrowWithMessage(TypeError, unaligned);
    expect(() => m.notify(65536, 1)).toThrowWithMessage(TypeError, outOfBounds);

    expect(() => m.wait32(0, 0, 0n)).toThrowWithMessage(TypeError, notShared);
    expect(() => m.wait32(0, 0, -1n)).toThrowWithMessage(TypeError, notShared);
    expect(() => m.wait32(2, 0, 0n)).toThrowWithMessage(TypeError, unaligned);
    expect(() => m.wait32(65536, 0, 0n)).toThrowWithMessage(TypeError, outOfBounds);
    expect(() => m.wait64(8, 0n, 0n)).toThrowWithMessage(TypeError, notShared);
    expect(() => m.wait64(4, 0n, 0n)).toThrowWithMessage(TypeError, unaligned);
    expect(() => m.wait64(65532, 0n, 0n)).toThrowWithMessage(TypeError, outOfBounds);

    m.fence();
});

test("atomic accesses can target memories other than the first", () => {
    const bin = readBinaryWasmFile("Fixtures/Modules/atomics-multi-memory.wasm");
    const module = parseWebAssemblyModule(bin);
    const store = module.getExport("store");
    const load = module.getExport("load");
    const loadFirst = module.getExport("load_first");

    module.invoke(store, 0, 0xdeadbeef);
    expect(module.invoke(load, 0)).toBe(0xdeadbeef | 0);
    expect(module.invoke(loadFirst, 0)).toBe(0);
});

test("validation rejects misaligned atomic memory arguments", () => {
    const bin = readBinaryWasmFile("Fixtures/Modules/atomics-misaligned.wasm");
    expect(() => validateWebAssemblyModule(bin)).toThrowWithMessage(SyntaxError, "atomic memory op alignment");
    validateWebAssemblyModule(readBinaryWasmFile("Fixtures/Modules/atomics.wasm"));
});
