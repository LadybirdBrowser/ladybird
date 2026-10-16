// JIT code computes Math.abs, Math.floor, Math.ceil, Math.round and
// Math.sqrt of numbers inline, and calls the runtime's functions of doubles for
// Math.exp, Math.log, Math.sin, Math.cos, Math.tan and Math.pow. These tests
// run every function many times, so that with a low JIT threshold they are
// compiled, and compare its results with the expected ones, -0, the
// infinities and the int32 range boundaries included.

const iterations = 100;

const functions = {
    abs: value => Math.abs(value),
    floor: value => Math.floor(value),
    ceil: value => Math.ceil(value),
    round: value => Math.round(value),
    sqrt: value => Math.sqrt(value),
};

// [argument, abs, floor, ceil, round, sqrt]
const cases = [
    [0, 0, 0, 0, 0, 0],
    [-0, 0, -0, -0, -0, -0],
    [7, 7, 7, 7, 7, Math.SQRT2 * Math.sqrt(3.5)],
    [-7, 7, -7, -7, -7, NaN],
    [2.5, 2.5, 2, 3, 3, 1.5811388300841898],
    [-2.5, 2.5, -3, -2, -2, NaN],
    [0.5, 0.5, 0, 1, 1, Math.SQRT1_2],
    [-0.5, 0.5, -1, -0, -0, NaN],
    [-0.4, 0.4, -1, -0, -0, NaN],
    [0.49999999999999994, 0.49999999999999994, 0, 1, 0, 0.7071067811865475],
    [-2147483648, 2147483648, -2147483648, -2147483648, -2147483648, NaN],
    [2147483647, 2147483647, 2147483647, 2147483647, 2147483647, 46340.950001051984],
    [2147483647.5, 2147483647.5, 2147483647, 2147483648, 2147483648, 46340.95000644678],
    [-2147483648.5, 2147483648.5, -2147483649, -2147483648, -2147483648, NaN],
    [1e300, 1e300, 1e300, 1e300, 1e300, 1e150],
    [Infinity, Infinity, Infinity, Infinity, Infinity, Infinity],
    [-Infinity, Infinity, -Infinity, -Infinity, -Infinity, NaN],
    [NaN, NaN, NaN, NaN, NaN, NaN],
];

describe("Math functions of one number in JIT code", () => {
    test("numbers", () => {
        const names = Object.keys(functions);
        const show = value => (Object.is(value, -0) ? "-0" : `${value}`);
        const mismatches = [];
        for (let i = 0; i < iterations; ++i) {
            for (const [argument, ...expected] of cases) {
                names.forEach((name, index) => {
                    const result = functions[name](argument);
                    if (!Object.is(result, expected[index]))
                        mismatches.push(`${name}(${show(argument)}) = ${show(result)}, not ${show(expected[index])}`);
                });
            }
        }
        expect(mismatches).toEqual([]);
    });

    test("values that are not numbers", () => {
        for (let i = 0; i < iterations; ++i) {
            expect(functions.abs("-3")).toBe(3);
            expect(functions.floor({ valueOf: () => 1.5 })).toBe(1);
            expect(functions.round(null)).toBe(0);
            expect(functions.sqrt(undefined)).toBeNaN();
            expect(functions.ceil(true)).toBe(1);
        }
    });

    test("replaced functions", () => {
        for (let i = 0; i < iterations; ++i) expect(functions.abs(-i)).toBe(i);
        const original = Math.abs;
        try {
            Math.abs = value => "replaced";
            expect(functions.abs(-1)).toBe("replaced");
        } finally {
            Math.abs = original;
        }
        expect(functions.abs(-1)).toBe(1);
    });
});

const computedFunctions = {
    exp: value => Math.exp(value),
    log: value => Math.log(value),
    sin: value => Math.sin(value),
    cos: value => Math.cos(value),
    tan: value => Math.tan(value),
};

// [argument, exp, log, sin, cos, tan]
const computedCases = [
    [0, 1, -Infinity, 0, 1, 0],
    [-0, 1, -Infinity, -0, 1, -0],
    [1, Math.E, 0, Math.sin(1), Math.cos(1), Math.tan(1)],
    [-1, 1 / Math.E, NaN, -Math.sin(1), Math.cos(1), -Math.tan(1)],
    [Infinity, Infinity, Infinity, NaN, NaN, NaN],
    [-Infinity, 0, NaN, NaN, NaN, NaN],
    [NaN, NaN, NaN, NaN, NaN, NaN],
];

// [base, exponent, power]
const powers = [
    [2, 10, 1024],
    [4, 0.5, 2],
    [-2, 3, -8],
    [3, -1, 1 / 3],
    [NaN, 0, 1],
    [NaN, -0, 1],
    [1, NaN, NaN],
    [1, Infinity, NaN],
    [-8, 1 / 3, NaN],
    [0, -1, Infinity],
    [-0, -1, -Infinity],
    [-0, -2, Infinity],
    [-0, 3, -0],
    [-Infinity, 3, -Infinity],
    [-Infinity, -3, -0],
    [0.5, -Infinity, Infinity],
    [2147483647, 1, 2147483647],
    [-2147483648, 1, -2147483648],
];

const pow = (base, exponent) => Math.pow(base, exponent);

describe("Math functions the runtime computes in JIT code", () => {
    test("numbers", () => {
        const names = Object.keys(computedFunctions);
        const show = value => (Object.is(value, -0) ? "-0" : `${value}`);
        const mismatches = [];
        for (let i = 0; i < iterations; ++i) {
            for (const [argument, ...expected] of computedCases) {
                names.forEach((name, index) => {
                    const result = computedFunctions[name](argument);
                    if (!Object.is(result, expected[index]))
                        mismatches.push(`${name}(${show(argument)}) = ${show(result)}, not ${show(expected[index])}`);
                });
            }
            for (const [base, exponent, expected] of powers) {
                const result = pow(base, exponent);
                if (!Object.is(result, expected))
                    mismatches.push(`pow(${show(base)}, ${show(exponent)}) = ${show(result)}, not ${show(expected)}`);
            }
        }
        expect(mismatches).toEqual([]);
    });

    test("values that are not numbers", () => {
        for (let i = 0; i < iterations; ++i) {
            expect(computedFunctions.exp("0")).toBe(1);
            expect(computedFunctions.log({ valueOf: () => 1 })).toBe(0);
            expect(computedFunctions.sin(null)).toBe(0);
            expect(computedFunctions.cos(undefined)).toBeNaN();
            expect(pow(true, "3")).toBe(1);
            expect(pow(2, { valueOf: () => 3 })).toBe(8);
        }
    });

    test("replaced functions", () => {
        for (let i = 0; i < iterations; ++i) expect(pow(i, 1)).toBe(i);
        const original = Math.pow;
        try {
            Math.pow = () => "replaced";
            expect(pow(2, 2)).toBe("replaced");
        } finally {
            Math.pow = original;
        }
        expect(pow(2, 2)).toBe(4);
    });
});
