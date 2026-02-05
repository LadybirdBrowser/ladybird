test("basic functionality", () => {
    const buffer = new ArrayBuffer(8);
    const view = new DataView(buffer);
    view.setInt8(0, -0x88);
    expect(view.getInt8(0)).toBe(0x78);
    view.setUint8(0, 0x88);
    expect(view.getUint8(0)).toBe(0x88);
});
