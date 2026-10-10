// Frames enter JIT code at function entry and at loop back edges, and leave
// it for the interpreter when a speculation fails, when a slow path does not
// continue in the compiled code, and when an exception is thrown. These tests
// run every function many times, so that with a low JIT threshold they are
// compiled, and then make them take every way out.

const iterations = 100;

describe("entering and leaving JIT code", () => {
    test("speculations that stop holding exit to the interpreter", () => {
        function add(a, b) {
            return a + b;
        }
        for (let i = 0; i < iterations; ++i) expect(add(i, 1)).toBe(i + 1);
        expect(add(0.5, 1)).toBe(1.5);
        expect(add("a", 1)).toBe("a1");
        expect(add(2147483647, 1)).toBe(2147483648);
        expect(add(-0, -0)).toBe(-0);
        expect(add({ valueOf: () => 4 }, 1)).toBe(5);
        for (let i = 0; i < iterations; ++i) expect(add(i, 0.25)).toBe(i + 0.25);
    });

    test("code that keeps exiting is recompiled with what the exits taught", () => {
        function scale(value, factor) {
            return value * factor;
        }
        for (let round = 0; round < 20; ++round) {
            for (let i = 0; i < iterations; ++i) expect(scale(i, 2)).toBe(2 * i);
            expect(scale(round + 0.5, 2)).toBe(2 * round + 1);
            expect(scale(65536 * 65536, 2)).toBe(2 * 65536 * 65536);
        }
    });

    test("loops continue in JIT code at their back edges", () => {
        function sum(count) {
            let total = 0;
            for (let i = 0; i < count; ++i) total += i % 13;
            return total;
        }
        let expected = 0;
        for (let i = 0; i < 100000; ++i) expected += i % 13;
        expect(sum(100000)).toBe(expected);

        function nested(count) {
            let total = 0;
            for (let i = 0; i < count; ++i) {
                for (let j = 0; j < 10; ++j) total += i ^ j;
            }
            return total;
        }
        let nested_expected = 0;
        for (let i = 0; i < 10000; ++i) {
            for (let j = 0; j < 10; ++j) nested_expected += i ^ j;
        }
        expect(nested(10000)).toBe(nested_expected);
    });

    test("loops that leave JIT code keep their state", () => {
        function collect(count) {
            const values = [];
            let total = 0;
            for (let i = 0; i < count; ++i) {
                total += i;
                if (i === count - 10) total += 0.5;
                values.push(total);
            }
            return values;
        }
        const values = collect(20000);
        expect(values.length).toBe(20000);
        expect(values[9989]).toBe((9989 * 9990) / 2);
        expect(values[19990]).toBe((19990 * 19991) / 2 + 0.5);
        expect(values[19999]).toBe((19999 * 20000) / 2 + 0.5);
    });

    test("stores that fill the holes of new arrays", () => {
        function fill(count, offset) {
            const values = new Array(count);
            for (let i = 0; i < count; ++i) values[i] = i + offset;
            return values;
        }
        for (let i = 0; i < iterations; ++i) {
            const values = fill(4, i);
            expect(values).toEqual([i, i + 1, i + 2, i + 3]);
        }
        const sparse = new Array(8);
        sparse[2] = "x";
        expect(fill(8, 0)[7]).toBe(7);
        expect(sparse[1]).toBeUndefined();
    });

    test("loops that left JIT code continue in it again", () => {
        function accumulate(count, odd_every) {
            let total = 0;
            for (let i = 0; i < count; ++i) {
                // NB: A path the loop did not take while it was profiled exits from its code each time it runs.
                if (i % odd_every === odd_every - 1) total += 0.5;
                else total += i & 7;
            }
            return total;
        }
        let expected = 0;
        for (let i = 0; i < 200000; ++i) expected += i % 50000 === 49999 ? 0.5 : i & 7;
        expect(accumulate(200000, 50000)).toBe(expected);
    });

    test("exceptions are caught in the compiled function", () => {
        function check(value) {
            if (value % 7 === 0) throw new Error(`bad ${value}`);
            return value;
        }
        function guarded(value) {
            try {
                return check(value) * 2;
            } catch (error) {
                return error.message;
            } finally {
                ++guarded.calls;
            }
        }
        guarded.calls = 0;
        for (let i = 0; i < iterations; ++i) expect(guarded(i)).toBe(i % 7 === 0 ? `bad ${i}` : 2 * i);
        expect(guarded.calls).toBe(iterations);
    });

    test("exceptions leave the compiled function for its caller", () => {
        function access(object) {
            return object.property.value;
        }
        function caller(object) {
            try {
                return access(object);
            } catch (error) {
                return error instanceof TypeError;
            }
        }
        for (let i = 0; i < iterations; ++i) expect(caller({ property: { value: i } })).toBe(i);
        expect(caller({})).toBeTrue();
        expect(caller(null)).toBeTrue();
        for (let i = 0; i < iterations; ++i) expect(caller({ property: { value: i } })).toBe(i);
    });

    test("constructors called from JIT code", () => {
        function Point(x, y) {
            this.x = x;
            this.y = y;
        }
        class Pair {
            constructor(first, second) {
                if (first === undefined) throw new RangeError("no first");
                this.first = first;
                this.second = second;
            }
        }
        function make(i) {
            const point = new Point(i, i + 1);
            const pair = new Pair(point, i);
            return pair.first.x + pair.first.y + pair.second;
        }
        function make_or_fail(i) {
            try {
                return new Pair(i === 13 ? undefined : i, i).first;
            } catch (error) {
                return error.name;
            }
        }
        for (let i = 0; i < iterations; ++i) {
            expect(make(i)).toBe(3 * i + 1);
            expect(make_or_fail(i)).toBe(i === 13 ? "RangeError" : i);
        }
    });

    test("recursion through JIT code", () => {
        function fibonacci(n) {
            return n < 2 ? n : fibonacci(n - 1) + fibonacci(n - 2);
        }
        expect(fibonacci(25)).toBe(75025);

        function depth(n) {
            return n === 0 ? 0 : 1 + depth(n - 1);
        }
        for (let i = 0; i < iterations; ++i) expect(depth(i)).toBe(i);
        expect(() => {
            depth(1e6);
        }).toThrowWithMessage(InternalError, "Call stack size limit exceeded");
        expect(depth(100)).toBe(100);
    });

    test("arguments objects of compiled functions", () => {
        function sloppy(a, b) {
            arguments[0] = a * 2;
            return a + b + arguments.length;
        }
        function strict(a, b) {
            "use strict";
            arguments[0] = a * 2;
            return a + b + arguments.length;
        }
        function rest() {
            return Array.prototype.slice.call(arguments, 1).length;
        }
        for (let i = 0; i < iterations; ++i) {
            expect(sloppy(i, 1)).toBe(2 * i + 3);
            expect(strict(i, 1)).toBe(i + 3);
            expect(sloppy(i, 1, 2)).toBe(2 * i + 4);
            expect(rest(i, i, i)).toBe(2);
        }
        expect(sloppy(0.5, 1)).toBe(4);
        expect(strict("x", 1)).toBe("x12");
    });

    test("generators run in the interpreter", () => {
        function* counter(count) {
            for (let i = 0; i < count; ++i) yield i;
        }
        function sum_of(count) {
            let total = 0;
            for (const value of counter(count)) total += value;
            return total;
        }
        for (let i = 0; i < iterations; ++i) expect(sum_of(i)).toBe((i * (i - 1)) / 2 + 0);
    });

    test("closures and environments of compiled functions", () => {
        function make_counter() {
            let count = 0;
            return () => ++count;
        }
        function use(counter, times) {
            let last = 0;
            for (let i = 0; i < times; ++i) last = counter();
            return last;
        }
        for (let i = 0; i < iterations; ++i) expect(use(make_counter(), i)).toBe(i);

        function shadowing(value) {
            let result = value;
            {
                let value = result + 1;
                result = () => value;
            }
            return result();
        }
        for (let i = 0; i < iterations; ++i) expect(shadowing(i)).toBe(i + 1);
    });
});
