// Loops whose values were only ever int32 values check values entering them from before the loop, and start the loop
// over in the interpreter where they are none. These tests run each loop often enough to be compiled with a low JIT
// threshold first.

const iterations = 100;

test("values entering a loop", () => {
    function sum(start, count) {
        let total = start;
        for (let i = 0; i < count; ++i) total = total + i;
        return total;
    }
    for (let i = 0; i < iterations; ++i) expect(sum(i, 10)).toBe(i + 45);
    expect(sum(0.5, 10)).toBe(45.5);
    expect(sum("a", 3)).toBe("a012");
    expect(sum(2 ** 31 - 1, 2)).toBe(2 ** 31);
    for (let i = 0; i < iterations; ++i) expect(sum(i, 3)).toBe(i + 3);
});

test("values entering a loop from different places", () => {
    function count(flag, start) {
        let n = flag ? start : 0;
        while (n < 20) ++n;
        return n;
    }
    for (let i = 0; i < iterations; ++i) {
        expect(count(true, i % 5)).toBe(20);
        expect(count(false, 7)).toBe(20);
    }
    expect(count(true, 19.5)).toBe(20.5);
    expect(count(true, "19")).toBe(20);
    expect(count(true, undefined)).toBeNaN();
});
