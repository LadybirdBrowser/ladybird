test("basic functionality", () => {
    let foo = 1;
    false && (foo = 2);
    expect(foo).toBe(1);

    foo = 1;
    true || (foo = 2);
    expect(foo).toBe(1);

    foo = 1;
    true ?? (foo = 2);
    expect(foo).toBe(1);
});
