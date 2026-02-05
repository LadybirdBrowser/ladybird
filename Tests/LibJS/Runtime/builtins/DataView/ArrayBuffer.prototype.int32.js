test("basic functionality", () => {
    const buffer = new ArrayBuffer(8);
    const view = new DataView(buffer);
    view.setInt32(0, -0xff08);
    expect(view.getInt32(0)).toBe(-0xff08);
    view.setInt32(0, -0xff08, true);
    expect(view.getInt32(0)).toBe(-0x7ff0001);
    view.setUint32(0, 0xff08);
    expect(view.getUint32(0)).toBe(0xff08);
    view.setUint32(0, 0xff08, true);
    expect(view.getUint32(0)).toBe(0x8ff0000);
});
