test("basic functionality", () => {
    expect(Symbol.prototype[Symbol.toStringTag]).toBe("Symbol");
});
