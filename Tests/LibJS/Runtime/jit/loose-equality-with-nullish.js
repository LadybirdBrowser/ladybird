// JIT code compares values loosely with undefined and null inline. These tests
// run every function many times, so that with a low JIT threshold they are
// compiled.

const iterations = 100;

const values = [
    undefined,
    null,
    0,
    1,
    -0,
    NaN,
    1.5,
    "",
    "null",
    "undefined",
    true,
    false,
    {},
    [],
    function () {},
    Symbol("s"),
    10n,
    0n,
];

describe("loose equality with undefined and null in JIT code", () => {
    test("against constants", () => {
        function equalsNull(value) {
            return value == null;
        }
        function notEqualsUndefined(value) {
            return value != undefined;
        }
        function nullEquals(value) {
            return null == value;
        }
        function branch(value) {
            if (value != null) return "some";
            return "none";
        }
        for (let i = 0; i < iterations; ++i) {
            for (const value of values) {
                const expected = value === undefined || value === null;
                expect(equalsNull(value)).toBe(expected);
                expect(notEqualsUndefined(value)).toBe(!expected);
                expect(nullEquals(value)).toBe(expected);
                expect(branch(value)).toBe(expected ? "none" : "some");
            }
        }
    });

    test("against values that are sometimes undefined or null", () => {
        function looselyEquals(a, b) {
            return a == b;
        }
        function looselyInequals(a, b) {
            if (a != b) return true;
            return false;
        }
        for (let i = 0; i < iterations; ++i) {
            for (const a of values) {
                for (const b of [undefined, null]) {
                    const expected = a === undefined || a === null;
                    expect(looselyEquals(a, b)).toBe(expected);
                    expect(looselyEquals(b, a)).toBe(expected);
                    expect(looselyInequals(a, b)).toBe(!expected);
                    expect(looselyInequals(b, a)).toBe(!expected);
                }
            }
            expect(looselyEquals("1", 1)).toBeTrue();
            expect(looselyEquals(1.5, 1.5)).toBeTrue();
            expect(looselyEquals({}, "[object Object]")).toBeTrue();
        }
    });
});
