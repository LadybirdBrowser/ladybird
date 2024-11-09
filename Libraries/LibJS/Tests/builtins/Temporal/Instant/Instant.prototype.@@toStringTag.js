test("basic functionality", () => {
    expect(Temporal.Instant.prototype[Symbol.toStringTag]).toBe("Temporal.Instant");
});
