test("basic multiplication", () => {
    expect(3 * 7).toBe(21);
    expect(-3 * 7).toBe(-21);
    expect(3 * -7).toBe(-21);
    expect(-3 * -7).toBe(21);
    expect(0 * 0).toBe(0);
    expect(1 * 0).toBe(0);
    expect(0 * 1).toBe(0);
});

test("multiplication with large integers", () => {
    expect(2147483647 * 2).toBe(4294967294);
    expect(-2147483648 * -1).toBe(2147483648);
    expect(2147483647 * 1).toBe(2147483647);
    expect(-2147483648 * 1).toBe(-2147483648);
});

test("negative zero from integer variable multiplication", () => {
    var a = -1;
    var b = 0;
    expect(a * b).toBe(-0);
    expect(b * a).toBe(-0);

    var c = -100;
    expect(c * b).toBe(-0);
    expect(b * c).toBe(-0);

    var d = -2147483648;
    expect(d * b).toBe(-0);
    expect(b * d).toBe(-0);
});

test("positive zero from non-negative integer variable multiplication", () => {
    var a = 1;
    var b = 0;
    expect(a * b).toBe(0);
    expect(b * a).toBe(0);
    expect(Object.is(a * b, -0)).toBeFalse();
    expect(Object.is(b * a, -0)).toBeFalse();

    var c = 0;
    expect(b * c).toBe(0);
    expect(Object.is(b * c, -0)).toBeFalse();
});

test("negative zero from compound assignment", () => {
    var a = -5;
    a *= 0;
    expect(a).toBe(-0);

    var b = 0;
    b *= -5;
    expect(b).toBe(-0);
});

test("positive zero from compound assignment", () => {
    var a = 5;
    a *= 0;
    expect(a).toBe(0);
    expect(Object.is(a, -0)).toBeFalse();
});

test("negative zero propagation through multiplication", () => {
    expect(-0 * 5).toBe(-0);
    expect(5 * -0).toBe(-0);
    expect(-0 * -5).toBe(0);
    expect(-5 * -0).toBe(0);
    expect(-0 * 0).toBe(-0);
    expect(0 * -0).toBe(-0);
    expect(-0 * -0).toBe(0);
});

test("negative zero detectable via division", () => {
    var a = -1;
    var b = 0;
    expect(1 / (a * b)).toBe(-Infinity);
    expect(1 / (b * a)).toBe(-Infinity);

    var c = 1;
    expect(1 / (c * b)).toBe(Infinity);
    expect(1 / (b * c)).toBe(Infinity);
});

test("multiplication with non-numeric types", () => {
    expect("3" * "7").toBe(21);
    expect("" * 5).toBe(0);
    expect(null * 5).toBe(0);
    expect(false * 5).toBe(0);
    expect(true * 5).toBe(5);
    expect([] * 5).toBe(0);
});

test("multiplication producing NaN", () => {
    expect(NaN * 5).toBeNaN();
    expect(5 * NaN).toBeNaN();
    expect(undefined * 5).toBeNaN();
    expect("foo" * 5).toBeNaN();
});

test("multiplication with Infinity", () => {
    expect(Infinity * 2).toBe(Infinity);
    expect(-Infinity * 2).toBe(-Infinity);
    expect(Infinity * -2).toBe(-Infinity);
    expect(-Infinity * -2).toBe(Infinity);
    expect(Infinity * Infinity).toBe(Infinity);
    expect(Infinity * -Infinity).toBe(-Infinity);
    expect(-Infinity * -Infinity).toBe(Infinity);

    // Infinity * 0 = NaN
    expect(Infinity * 0).toBeNaN();
    expect(0 * Infinity).toBeNaN();
    expect(-Infinity * 0).toBeNaN();
    expect(0 * -Infinity).toBeNaN();
});
