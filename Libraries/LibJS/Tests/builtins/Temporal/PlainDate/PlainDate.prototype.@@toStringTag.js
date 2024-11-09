test("basic functionality", () => {
    expect(Temporal.PlainDate.prototype[Symbol.toStringTag]).toBe("Temporal.PlainDate");
});
