test("basic functionality", () => {
    expect(Temporal.Duration.prototype[Symbol.toStringTag]).toBe("Temporal.Duration");
});
