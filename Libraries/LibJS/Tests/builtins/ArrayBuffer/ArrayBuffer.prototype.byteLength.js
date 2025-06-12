test("basic functionality", () => {
    expect(new ArrayBuffer().byteLength).toBe(0);
    expect(new ArrayBuffer(1).byteLength).toBe(1);
    expect(new ArrayBuffer(64).byteLength).toBe(64);
    expect(new ArrayBuffer(123).byteLength).toBe(123);
});
