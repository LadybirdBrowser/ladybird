test("basic functionality", () => {
    const foo = {
        ำ: 12389,
    };

    expect(foo.ำ).toBe(12389);
});
