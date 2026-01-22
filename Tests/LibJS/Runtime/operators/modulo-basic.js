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
