test("invariants", () => {
    expect(Atomics.pause).toHaveLength(0);
});

test("basic functionality", () => {
    expect(Atomics.pause()).toBeUndefined();
});
