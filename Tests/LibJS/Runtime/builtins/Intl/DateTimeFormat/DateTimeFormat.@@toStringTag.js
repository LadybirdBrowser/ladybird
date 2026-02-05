test("basic functionality", () => {
    expect(Intl.DateTimeFormat.prototype[Symbol.toStringTag]).toBe("Intl.DateTimeFormat");
});
