test("basic functionality", () => {
    // Ensuring it's the same function as the global
    // parseInt() is enough as that already has tests :^)
    expect(Number.parseInt).toBe(parseInt);
});
