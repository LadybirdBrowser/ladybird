test("basic functionality", () => {
    expect(typeof Boolean.prototype).toBe("object");
    expect(Boolean.prototype.valueOf()).toBeFalse();
    expect(Boolean.prototype).not.toHaveProperty("length");
});
