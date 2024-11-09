test("basic functionality", () => {
    expect(typeof Number.prototype).toBe("object");
    expect(Number.prototype.valueOf()).toBe(0);
});
