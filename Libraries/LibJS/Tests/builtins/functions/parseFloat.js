// This is necessary because we can't simply check if 0 is negative using equality,
// since -0 === 0 evaluates to true.
// test262 checks for negative zero in a similar way here:
// https://github.com/tc39/test262/blob/main/test/built-ins/parseFloat/S15.1.2.3_A1_T2.js
function isZeroNegative(x) {
    const isZero = x === 0;
    const isNegative = 1 / x === Number.NEGATIVE_INFINITY;

    return isZero && isNegative;
}

test("parsing numbers", () => {
    [
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

test("parsing zero", () => {
    expect(isZeroNegative(parseFloat("0"))).toBeFalse();
    expect(isZeroNegative(parseFloat("+0"))).toBeFalse();
    expect(isZeroNegative(parseFloat("-0"))).toBeTrue();

    expect(isZeroNegative(parseFloat(0))).toBeFalse();
    expect(isZeroNegative(parseFloat(+0))).toBeFalse();
    expect(isZeroNegative(parseFloat(-0))).toBeFalse();
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
