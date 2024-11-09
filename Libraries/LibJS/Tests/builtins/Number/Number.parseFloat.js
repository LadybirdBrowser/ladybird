test("basic functionality", () => {
    // Ensuring it's the same function as the global
    // parseFloat() is enough as that already has tests :^)
    expect(Number.parseFloat).toBe(parseFloat);
});
