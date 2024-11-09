test("basic functionality", () => {
    expect(DataView).toHaveLength(1);
    expect(DataView.name).toBe("DataView");
    expect(DataView.prototype.constructor).toBe(DataView);

    const buffer = new ArrayBuffer();
    expect(new DataView(buffer)).toBeInstanceOf(DataView);
    expect(typeof new DataView(buffer)).toBe("object");
});

test("DataView constructor must be invoked with 'new'", () => {
    expect(() => {
        DataView();
    }).toThrowWithMessage(TypeError, "DataView constructor must be called with 'new'");
});
