test("basic functionality", () => {
    expect(BigInt.prototype[Symbol.toStringTag]).toBe("BigInt");
});
