function compileCalls(f, ...args) {
    jit.prepare(f);
    for (let i = 0; i < 10; ++i) f(...args);
    expect(jit.compile(f)).toBe(jit.enabled);
    return f;
}

test("int32 arguments", () => {
    const min = compileCalls((a, b) => Math.min(a, b), 1, 2);
    const max = compileCalls((a, b) => Math.max(a, b), 1, 2);
    expect(min(3, -4)).toBe(-4);
    expect(max(3, -4)).toBe(3);
    expect(min(-2147483648, 2147483647)).toBe(-2147483648);
    expect(max(-2147483648, 2147483647)).toBe(2147483647);
    expect(min(5, 5)).toBe(5);
    expect(jit.exitCount(min)).toBe(0);
    expect(jit.exitCount(max)).toBe(0);
});

test("other arguments exit and get the full semantics", () => {
    const min = compileCalls((a, b) => Math.min(a, b), 1, 2);
    const max = compileCalls((a, b) => Math.max(a, b), 1, 2);
    expect(min(1.5, 2)).toBe(1.5);
    expect(max(0, -0)).toBe(0);
    expect(Object.is(min(0, -0), -0)).toBeTrue();
    expect(min(NaN, 1)).toBeNaN();
    expect(max("3", 2)).toBe(3);
    let conversions = 0;
    const counted = {
        valueOf() {
            ++conversions;
            return 9;
        },
    };
    expect(max(counted, 1)).toBe(9);
    expect(conversions).toBe(1);
});

test("replaced functions are called", () => {
    const min = compileCalls((a, b) => Math.min(a, b), 1, 2);
    const original = Math.min;
    Math.min = () => "replaced";
    try {
        expect(min(1, 2)).toBe("replaced");
    } finally {
        Math.min = original;
    }
    expect(min(1, 2)).toBe(1);
});

test("sites with other results call the function", () => {
    const max = compileCalls((a, b) => Math.max(a, b), 0.5, 2.5);
    expect(max(1.5, 0.25)).toBe(1.5);
    expect(max(3, 4)).toBe(4);
    expect(jit.exitCount(max)).toBe(0);
});

test("constant arguments", () => {
    const clampLow = compileCalls(a => Math.max(a, 0), 3);
    expect(clampLow(-3)).toBe(0);
    expect(clampLow(7)).toBe(7);
    expect(jit.exitCount(clampLow)).toBe(0);

    const clampHigh = compileCalls(a => Math.min(a, 2.5), 1);
    expect(clampHigh(2)).toBe(2);
    expect(clampHigh(3)).toBe(2.5);
    expect(jit.exitCount(clampHigh)).toBe(0);
});
