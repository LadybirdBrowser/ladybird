test("basic functionality", () => {
    expect(Promise.prototype[Symbol.toStringTag]).toBe("Promise");
});
