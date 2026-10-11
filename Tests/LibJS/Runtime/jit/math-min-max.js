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

test("sites with other results compute with doubles", () => {
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

test("double arguments", () => {
    const min = compileCalls((a, b) => Math.min(a, b), 1.5, 2);
    const max = compileCalls((a, b) => Math.max(a, b), 2.5, 1);
    expect(min(1.5, -2.25)).toBe(-2.25);
    expect(max(1.5, -2.25)).toBe(1.5);
    expect(min(3, 4)).toBe(3);
    expect(Object.is(min(0, -0), -0)).toBeTrue();
    expect(Object.is(min(-0, 0), -0)).toBeTrue();
    expect(Object.is(max(0, -0), 0)).toBeTrue();
    expect(Object.is(max(-0, -0), -0)).toBeTrue();
    expect(min(NaN, 1)).toBeNaN();
    expect(max(1, NaN)).toBeNaN();
    expect(min(-Infinity, Infinity)).toBe(-Infinity);
    expect(max(-Infinity, Infinity)).toBe(Infinity);
    expect(jit.exitCount(min)).toBe(0);
    expect(jit.exitCount(max)).toBe(0);
});

test("several arguments", () => {
    const min3 = compileCalls((a, b, c) => Math.min(a, b, c), 1, 2, 3);
    const max4 = compileCalls((a, b, c, d) => Math.max(a, b, c, d), 1.5, 2, 3, 4);
    const min1 = compileCalls(a => Math.min(a), 7);
    expect(min3(5, -1, 3)).toBe(-1);
    expect(min3(5, 7, 3)).toBe(3);
    expect(max4(1, 2.5, -3, 0)).toBe(2.5);
    expect(max4(1, NaN, -3, 0)).toBeNaN();
    expect(min1(-4)).toBe(-4);
    expect(min1(2.5)).toBe(2.5);
});

test("arguments are converted in order before any comparison", () => {
    const max = compileCalls((a, b) => Math.max(a, b), 1.5, 2);
    const order = [];
    const counted = (name, value) => ({
        valueOf() {
            order.push(name);
            return value;
        },
    });
    expect(max(NaN, counted("b", 4))).toBeNaN();
    expect(order).toEqual(["b"]);
    expect(max(counted("a", 1), counted("b", 4))).toBe(4);
    expect(order).toEqual(["b", "a", "b"]);
    expect(max("2.5", 1)).toBe(2.5);
});

test("sites that see other arguments after compiling compute them correctly", () => {
    const max = compileCalls((a, b) => Math.max(a, b), 2.5, 1);
    for (let i = 0; i < 100; ++i) {
        expect(max("3", "10")).toBe(10);
        expect(max(undefined, 1)).toBeNaN();
        expect(max(null, -1)).toBe(0);
    }
    expect(max(0.5, -1.5)).toBe(0.5);
});
