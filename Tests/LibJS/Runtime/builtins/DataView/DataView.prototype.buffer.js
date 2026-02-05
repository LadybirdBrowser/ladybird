test("basic functionality", () => {
    const buffer = new ArrayBuffer();
    expect(new DataView(buffer).buffer).toBe(buffer);
});
