test("basic functionality", () => {
    const buffer = new ArrayBuffer(8);
    const view = new DataView(buffer);
    view.setFloat32(0, 6854.162);
    expect(view.getFloat32(0)).toBe(6854.162109375);
    view.setFloat32(0, 6854.162, true);
    expect(view.getFloat32(0)).toBe(46618900);
});
