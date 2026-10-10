// JIT code branches on the truthiness of strings and doubles inline. These
// tests run every function many times, so that with a low JIT threshold they
// are compiled.

const iterations = 100;

function truthy(value) {
    if (value) return true;
    return false;
}

function negated(value) {
    return !value;
}

describe("Truthiness in JIT code", () => {
    test("strings", () => {
        const long = "a long string, ".repeat(3);
        const cases = [
            ["", false],
            ["a", true],
            ["0", true],
            ["false", true],
            [long, true],
            [long + long, true],
            [long.substring(5, 5), false],
            [long.substring(5, 25), true],
            ["é", true],
            [String.fromCharCode(0), true],
        ];
        for (let i = 0; i < iterations; ++i) {
            for (const [value, expected] of cases) {
                expect(truthy(value)).toBe(expected);
                expect(negated(value)).toBe(!expected);
            }
        }
    });

    test("doubles", () => {
        const cases = [
            [0.5, true],
            [-1.5, true],
            [0, false],
            [-0, false],
            [NaN, false],
            [Infinity, true],
            [-Infinity, true],
            [Number.MIN_VALUE, true],
            [-Number.MIN_VALUE, true],
            [2 ** 40, true],
        ];
        for (let i = 0; i < iterations; ++i) {
            for (const [value, expected] of cases) {
                expect(truthy(value)).toBe(expected);
                expect(negated(value)).toBe(!expected);
            }
        }
    });

    test("other values", () => {
        const cases = [
            [Symbol("s"), true],
            [0n, false],
            [1n, true],
            [undefined, false],
            [null, false],
            [{}, true],
            [[], true],
            [() => {}, true],
        ];
        for (let i = 0; i < iterations; ++i) {
            for (const [value, expected] of cases) {
                expect(truthy(value)).toBe(expected);
                expect(negated(value)).toBe(!expected);
            }
        }
    });
});
