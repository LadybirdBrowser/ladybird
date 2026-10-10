// Inlined callees read their reserved registers, such as `this`, in blocks
// after branches. These tests run every function many times, so that with
// a low JIT threshold the callees are inlined into their callers.

const iterations = 100;

test("this after a branch in an inlined method", () => {
    class Box {
        constructor(value) {
            this.value = value;
        }
        valueOr(fallback) {
            if (fallback) return this.value + fallback;
            return this.value - 1;
        }
    }
    function caller(box, i) {
        return box.valueOr(i % 2);
    }
    const box = new Box(10);
    for (let i = 0; i < iterations; ++i) expect(caller(box, i)).toBe(i % 2 ? 11 : 9);
});
