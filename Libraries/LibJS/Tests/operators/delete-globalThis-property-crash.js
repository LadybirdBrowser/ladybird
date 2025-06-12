a = 1;

test("basic functionality", () => {
    expect(delete globalThis.a).toBeTrue();
    expect(() => {
        a = 2;
    }).not.toThrow();
});
