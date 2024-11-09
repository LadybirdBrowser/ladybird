test("basic functionality", () => {
    const o = {};
    o.a = 1;

    expect(o.a === 1).toBeTrue();
    expect(!o.a === false).toBeTrue();
    expect(!o.a === !o.a).toBeTrue();
    expect(~o.a === ~o.a).toBeTrue();
    expect(+o.a === +o.a).toBeTrue();
    expect(-o.a === -o.a).toBeTrue();

    expect((typeof "x" === "string") === true).toBeTrue();
    expect(!(typeof "x" === "string") === false).toBeTrue();
});

test("unary +/- operators bind higher than binary", () => {
    expect(10 ** -3 / 2).toEqual(0.0005);
});
