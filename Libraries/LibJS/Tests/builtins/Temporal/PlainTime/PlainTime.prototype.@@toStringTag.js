test("basic functionality", () => {
    expect(Temporal.PlainTime.prototype[Symbol.toStringTag]).toBe("Temporal.PlainTime");
});
