test("basic functionality", () => {
    var a = 1,
        b = 2,
        c = a + b;
    expect(a).toBe(1);
    expect(b).toBe(2);
    expect(c).toBe(3);
});
