test("basic functionality", () => {
    expect(Temporal.Now[Symbol.toStringTag]).toBe("Temporal.Now");
});
