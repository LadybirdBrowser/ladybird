test("basic functionality", () => {
    expect(Math[Symbol.toStringTag]).toBe("Math");
    expect(Math.toString()).toBe("[object Math]");
});
