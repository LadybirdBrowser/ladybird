test("basic functionality", () => {
    const buffer = new ArrayBuffer(8);
    const view = new DataView(buffer);
    view.setInt16(0, -0xff8);
    expect(view.getInt16(0)).toBe(-0xff8);
    view.setInt16(0, -0xff8, true);
    expect(view.getInt16(0)).toBe(0x8f0);
    view.setUint16(0, 0xff8);
    expect(view.getUint16(0)).toBe(0xff8);
    view.setUint16(0, 0xff8, true);
    expect(view.getUint16(0)).toBe(0xf80f);
});
