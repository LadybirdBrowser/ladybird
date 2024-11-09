test("basic functionality", () => {
    expect(Temporal.PlainDateTime.prototype[Symbol.toStringTag]).toBe("Temporal.PlainDateTime");
});
