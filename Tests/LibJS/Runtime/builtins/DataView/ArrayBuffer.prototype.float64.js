test("basic functionality", () => {
    const buffer = new ArrayBuffer(8);
    const view = new DataView(buffer);
    view.setFloat64(0, 6865415254.161212);
    expect(view.getFloat64(0)).toBe(6865415254.161212);
    view.setFloat64(0, 6865415254.161212, true);
    expect(view.getFloat64(0)).toBe(4.2523296908380052e94);
});
