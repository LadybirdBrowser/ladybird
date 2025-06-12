test("inside parenthesis", () => {
    expect((1, 2, 3)).toBe(3);
    expect((1, 2 + 3, 4)).toBe(4);
});

test("with post-increment operator", () => {
    let foo = 0;
    foo = (foo++, foo);
    expect(foo).toBe(1);
});

test("with assignment operator", () => {
    var a, b, c;
    expect(((a = b = 3), (c = 4))).toBe(4);
    expect(a).toBe(3);
    expect(b).toBe(3);
    expect(c).toBe(4);

    var x, y, z;
    expect((x = ((y = 5), (z = 6)))).toBe(6);
    expect(x).toBe(6);
    expect(y).toBe(5);
    expect(z).toBe(6);
});
