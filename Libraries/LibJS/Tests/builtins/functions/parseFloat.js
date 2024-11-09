test("parsing numbers", () => {
    [
        [0, 0],
        [1, 1],
        [0.23, 0.23],
        [1.23, 1.23],
        [0.0123e2, 1.23],
        [1.23e4, 12300],
        [Infinity, Infinity],
    ].forEach(test => {
        expect(parseFloat(test[0])).toBe(test[1]);
        expect(parseFloat(+test[0])).toBe(test[1]);
        expect(parseFloat(-test[0])).toBe(-test[1]);
    });
});

test("parsing strings", () => {
    [
        ["0", 0],
        ["1", 1],
        [".23", 0.23],
        ["1.23", 1.23],
        ["0.0123E+2", 1.23],
        ["1.23e4", 12300],
        ["0x1.23p5", 0],
        ["1.23p5", 1.23],
        ["1.23e42351245", Infinity],
        ["Infinity", Infinity],
    ].forEach(test => {
        expect(parseFloat(test[0])).toBe(test[1]);
        expect(parseFloat(`+${test[0]}`)).toBe(test[1]);
        expect(parseFloat(`-${test[0]}`)).toBe(-test[1]);
        expect(parseFloat(`${test[0]}foo`)).toBe(test[1]);
        expect(parseFloat(`+${test[0]}foo`)).toBe(test[1]);
        expect(parseFloat(`-${test[0]}foo`)).toBe(-test[1]);
        expect(parseFloat(`   \n  \t ${test[0]} \v  foo   `)).toBe(test[1]);
        expect(parseFloat(`   \r -${test[0]} \f \n\n  foo   `)).toBe(-test[1]);
        expect(parseFloat({ toString: () => test[0] })).toBe(test[1]);
    });
});

test("parsing NaN", () => {
    [
        "",
        [],
        [],
        true,
        false,
        null,
        undefined,
        NaN,
        "foo123",
        "foo+123",
        "fooInfinity",
        "foo+Infinity",
    ].forEach(value => {
        expect(parseFloat(value)).toBeNaN();
    });

    expect(parseFloat()).toBeNaN();
    expect(parseFloat("", 123, Infinity)).toBeNaN();
});
