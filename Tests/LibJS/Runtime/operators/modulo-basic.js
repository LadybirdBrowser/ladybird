test("basic functionality", () => {
    expect(10 % 3).toBe(1);
    expect(10.5 % 2.5).toBe(0.5);
    expect(-0.99 % 0.99).toBe(-0);

    // Examples from MDN:
    // https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Operators/Arithmetic_Operators
    expect(12 % 5).toBe(2);
    expect(-1 % 2).toBe(-1);
    expect(1 % -2).toBe(1);
    expect(1 % 2).toBe(1);
    expect(2 % 3).toBe(2);
    expect(-4 % 2).toBe(-0);
    expect(5.5 % 2).toBe(1.5);
    expect(NaN % 2).toBeNaN();
    expect(2 % NaN).toBeNaN();
    expect(NaN % NaN).toBeNaN();
    expect(Infinity % 1).toBeNaN();
    expect(-Infinity % 1).toBeNaN();
    expect(1 % Infinity).toBe(1);
    expect(1 % -Infinity).toBe(1);
    expect(1 % 0).toBeNaN();
    expect(1 % -0).toBeNaN();
    expect(0 % 5).toBe(0);
    expect(-0 % 5).toBe(-0);
    expect(-1 % -1).toBe(-0);

    // test262 examples
    expect(1 % null).toBeNaN();
    expect(null % 1).toBe(0);
    expect(true % null).toBeNaN();
    expect(null % true).toBe(0);
    expect("1" % null).toBeNaN();
    expect(null % "1").toBe(0);
    expect(null % undefined).toBeNaN();
    expect(undefined % null).toBeNaN();
    expect(undefined % undefined).toBeNaN();
    expect(null % null).toBeNaN();
});

test("Int32 fast path edge cases", () => {
    // INT32_MIN % -1 edge case (would be UB in C++ without special handling)
    // Result is -0 because dividend is negative
    expect(-2147483648 % -1).toBe(-0);
    expect(-2147483648 % 1).toBe(-0);

    // INT32_MAX cases
    expect(2147483647 % 2).toBe(1);
    expect(2147483647 % -2).toBe(1);
    expect(-2147483647 % 2).toBe(-1);
    expect(-2147483647 % -2).toBe(-1);

    // INT32_MIN cases (result is -0 when remainder is 0 and dividend is negative)
    expect(-2147483648 % 2).toBe(-0);
    expect(-2147483648 % -2).toBe(-0);
    expect(-2147483648 % 3).toBe(-2);
    expect(-2147483648 % -3).toBe(-2);

    // Division by zero (Int32 path)
    expect(1 % 0).toBeNaN();
    expect(-1 % 0).toBeNaN();
    expect(2147483647 % 0).toBeNaN();
    expect(-2147483648 % 0).toBeNaN();

    // Small Int32 values
    expect(7 % 3).toBe(1);
    expect(-7 % 3).toBe(-1);
    expect(7 % -3).toBe(1);
    expect(-7 % -3).toBe(-1);

    // Self-modulo
    expect(100 % 100).toBe(0);
    expect(-100 % -100).toBe(-0);
    expect(-100 % 100).toBe(-0);
    expect(100 % -100).toBe(0);

    // Larger divisor than dividend
    expect(3 % 7).toBe(3);
    expect(-3 % 7).toBe(-3);
    expect(3 % -7).toBe(3);
    expect(-3 % -7).toBe(-3);
});

test("integer-valued double operands", () => {
    const cases = [
        [2147483648, 3],
        [2147483647 * 16807, 2147483647],
        [2147483646 * 16807, 2147483647],
        [2 ** 40 + 1, 2 ** 32 + 1],
        [Number.MAX_SAFE_INTEGER, 3],
        [2 ** 53, 3],
        [2 ** 63 - 1024, 3],
        [2 ** 63 - 1024, 2 ** 62],
        [2 ** 63 - 1024, 2 ** 63 - 1024],
        [1, 2 ** 63 - 1024],
        [0, 2 ** 32],
        // Exercise the fallback at and beyond the conversion boundary too.
        [2 ** 63, 1],
        [2 ** 63, 3],
        [2 ** 63, 2 ** 63],
        [2 ** 64, 3],
        [2 ** 64, 2 ** 63],
        [1, 2 ** 63],
        [Number.MAX_VALUE, 3],
    ];

    for (const [dividend, divisor] of cases) {
        const remainder = Number(BigInt(dividend) % BigInt(divisor));
        expect(dividend % divisor).toBe(remainder);
        expect(dividend % -divisor).toBe(remainder);
        expect(-dividend % divisor).toBe(-remainder);
        expect(-dividend % -divisor).toBe(-remainder);
    }
});

test("non-integral double operands and special values", () => {
    const cases = [
        [2147483648.5, 3, 2.5],
        [2147483648, 2.5, 0.5],
        [2147483648.5, 2.5, 1],
        [2147483648.5, 0.5, 0],
        [0.5, 2147483648, 0.5],
        [Number.MIN_VALUE, 2147483648, Number.MIN_VALUE],
        [2147483648, Number.MIN_VALUE, 0],
        [2147483648, Infinity, 2147483648],
    ];

    for (const [dividend, divisor, remainder] of cases) {
        expect(dividend % divisor).toBe(remainder);
        expect(dividend % -divisor).toBe(remainder);
        expect(-dividend % divisor).toBe(-remainder);
        expect(-dividend % -divisor).toBe(-remainder);
    }

    for (const dividend of [2147483648, -2147483648, 2 ** 63, -(2 ** 63)]) {
        expect(dividend % 0).toBeNaN();
        expect(dividend % -0).toBeNaN();
        expect(dividend % NaN).toBeNaN();
        expect(NaN % dividend).toBeNaN();
        expect(Infinity % dividend).toBeNaN();
        expect(-Infinity % dividend).toBeNaN();
    }
});
