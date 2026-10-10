// Exits inside calls the JIT inlined rebuild the frames of every inlined
// callee, from the outermost to the innermost, and the interpreter continues
// in the innermost one. These tests run every caller and callee many times,
// so that with a low JIT threshold the callees are inlined into their
// callers, and then make the speculations inside the callees fail.

const iterations = 100;

describe("exits in inlined calls", () => {
    test("an exit at the entry of an inlined callee", () => {
        function callee(value) {
            return value * 3;
        }
        function caller(value) {
            return callee(value) + 1;
        }
        for (let i = 0; i < iterations; ++i) expect(caller(i)).toBe(3 * i + 1);
        expect(caller(0.5)).toBe(2.5);
        expect(caller("2")).toBe(7);
        for (let i = 0; i < iterations; ++i) expect(caller(i)).toBe(3 * i + 1);
    });

    test("an exit after the entry of an inlined callee keeps its locals and arguments", () => {
        function callee(object, offset) {
            const doubled = offset * 2;
            const sum = object.value + doubled;
            return [sum, doubled, offset, object];
        }
        function caller(object, offset) {
            const before = offset + 1;
            const result = callee(object, offset);
            return [before, ...result];
        }
        for (let i = 0; i < iterations; ++i) {
            const object = { value: i };
            expect(caller(object, 2)).toEqual([3, i + 4, 4, 2, object]);
        }
        const odd_shape = { other: true, value: 10 };
        expect(caller(odd_shape, 2)).toEqual([3, 14, 4, 2, odd_shape]);
        const double_value = { value: 0.25 };
        expect(caller(double_value, 2)).toEqual([3, 4.25, 4, 2, double_value]);
    });

    test("exits several inlined calls deep", () => {
        function innermost(value) {
            return value.amount * 2;
        }
        function middle(value) {
            return innermost(value) + 1;
        }
        function outer(value) {
            return middle(value) + 10;
        }
        function caller(value) {
            return outer(value) * 2;
        }
        for (let i = 0; i < iterations; ++i) expect(caller({ amount: i })).toBe((2 * i + 11) * 2);
        expect(caller({ amount: 1.5 })).toBe(28);
        expect(caller({ extra: 1, amount: 2 })).toBe(30);
        expect(caller({ amount: "3" })).toBe(34);
    });

    test("values that only an exit refers to survive garbage collection", () => {
        function callee(holder, count) {
            const made = { count };
            if (count < 0) return made;
            return holder.value + made.count;
        }
        function caller(holder, count) {
            const kept = { kept: count };
            const result = callee(holder, count);
            gc();
            return [result, kept.kept];
        }
        for (let i = 0; i < iterations; ++i) expect(caller({ value: 1 }, i)).toEqual([i + 1, i]);
        expect(caller({ value: 0.5 }, 2)).toEqual([2.5, 2]);
        expect(caller({ value: 1 }, -1)).toEqual([{ count: -1 }, -1]);
    });

    test("exceptions thrown by slow paths in inlined callees", () => {
        function callee(object) {
            return object.inner.value;
        }
        function caller(object) {
            try {
                return callee(object);
            } catch (error) {
                return error instanceof TypeError ? "TypeError" : "other";
            }
        }
        for (let i = 0; i < iterations; ++i) expect(caller({ inner: { value: i } })).toBe(i);
        expect(caller({})).toBe("TypeError");
        expect(caller({ inner: null })).toBe("TypeError");
        for (let i = 0; i < iterations; ++i) expect(caller({ inner: { value: i } })).toBe(i);
    });

    test("inlined callees that read their arguments object", () => {
        function count_arguments() {
            return arguments.length + arguments[0];
        }
        function caller(value) {
            return count_arguments(value, 1, 2);
        }
        for (let i = 0; i < iterations; ++i) expect(caller(i)).toBe(i + 3);
        expect(caller(0.5)).toBe(3.5);
        expect(caller("x")).toBe("3x");
    });

    test("calls through call, apply and bound functions", () => {
        function target(a, b) {
            return a * 10 + b;
        }
        const bound = target.bind(null, 7);
        function via_call(value) {
            return target.call(null, value, 1);
        }
        function via_apply(value) {
            return target.apply(null, [value, 2]);
        }
        function via_bound(value) {
            return bound(value);
        }
        for (let i = 0; i < iterations; ++i) {
            expect(via_call(i)).toBe(10 * i + 1);
            expect(via_apply(i)).toBe(10 * i + 2);
            expect(via_bound(i)).toBe(70 + i);
        }
        expect(via_call(0.5)).toBe(6);
        expect(via_apply("1")).toBe(12);
        expect(via_bound(0.5)).toBe(70.5);
    });
});
