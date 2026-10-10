// The slow paths of arithmetic that only ever saw numbers make numbers too, which compiled code takes as such. Where
// they do not (bigints, and objects that convert to them), the code continues in the interpreter after the
// instruction. These tests run each site often enough to be compiled with a low JIT threshold first.

const iterations = 100;

test("binary operations", () => {
    function subtract(a, b) {
        return a - b;
    }
    function multiply(a, b) {
        return a * b;
    }
    function add(a, b) {
        return a + b;
    }
    for (let i = 0; i < iterations; ++i) {
        expect(subtract(i, 0.5)).toBe(i - 0.5);
        expect(multiply(i, 1.5)).toBe(i * 1.5);
        expect(add(i, 0.25)).toBe(i + 0.25);
    }
    expect(subtract(10n, 3n)).toBe(7n);
    expect(multiply(4n, 5n)).toBe(20n);
    expect(add(1n, 2n)).toBe(3n);
    expect(subtract({ valueOf: () => 7n }, 2n)).toBe(5n);
    expect(subtract({ valueOf: () => 7 }, 2)).toBe(5);
    expect(add(true, 1)).toBe(2);
});

test("unary operations and updates", () => {
    function negate(value) {
        return -value;
    }
    function increment(value) {
        let result = value;
        result++;
        return result;
    }
    function postfix(value) {
        let result = value;
        const old = result--;
        return [old, result];
    }
    for (let i = 0; i < iterations; ++i) {
        expect(negate(i + 0.5)).toBe(-(i + 0.5));
        expect(increment(i + 0.5)).toBe(i + 1.5);
        expect(postfix(i + 0.5)).toEqual([i + 0.5, i - 0.5]);
    }
    expect(negate(5n)).toBe(-5n);
    expect(increment(5n)).toBe(6n);
    expect(postfix(5n)).toEqual([5n, 4n]);
    expect(negate("3")).toBe(-3);
    expect(increment("3")).toBe(4);
});

test("loops whose values turn into bigints", () => {
    function sumOfSquares(values, total) {
        for (let i = 0; i < values.length; ++i) total = total + values[i] * values[i];
        return total;
    }
    const numbers = [0.5, 1.5, 2.5];
    for (let i = 0; i < iterations; ++i) expect(sumOfSquares(numbers, 0)).toBe(8.75);
    expect(sumOfSquares([1n, 2n, 3n], 0n)).toBe(14n);
    expect(sumOfSquares(numbers, 0)).toBe(8.75);
});
