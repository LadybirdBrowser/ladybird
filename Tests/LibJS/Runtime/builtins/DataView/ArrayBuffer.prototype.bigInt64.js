test("basic functionality", () => {
    const buffer = new ArrayBuffer(8);
    const view = new DataView(buffer);
    view.setBigInt64(0, -0x6f80ff08n);
    expect(view.getBigInt64(0)).toBe(-0x6f80ff08n);
    view.setBigInt64(0, -0x6f80ff08n, true);
    expect(view.getBigInt64(0)).toBe(-0x7ff806f00000001n);
    view.setBigUint64(0, 0x6f80ff08n);
    expect(view.getBigUint64(0)).toBe(0x6f80ff08n);
    view.setBigUint64(0, 0x6f80ff08n, true);
    expect(view.getBigUint64(0)).toBe(0x8ff806f00000000n);
});
