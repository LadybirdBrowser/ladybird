test("basic functionality", () => {
    expect(Atomics[Symbol.toStringTag]).toBe("Atomics");
    expect(Atomics.toString()).toBe("[object Atomics]");
});
