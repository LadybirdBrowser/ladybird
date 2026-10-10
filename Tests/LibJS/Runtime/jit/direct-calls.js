// JIT code calls the single callee a call site saw directly when it does not
// inline it. These tests run every caller and callee many times, so that with
// a low JIT threshold they are compiled.

const iterations = 100;

describe("direct calls from JIT code", () => {
    test("arguments, missing arguments and return values", () => {
        function callee(a, b, c) {
            if (a < 0) return;
            return [a, b, c, arguments.length];
        }
        function caller(i) {
            return [callee(i), callee(i, 1, 2, 3), callee(-1, 2)];
        }
        for (let i = 0; i < iterations; ++i) {
            expect(caller(i)).toEqual([[i, undefined, undefined, 1], [i, 1, 2, 4], undefined]);
        }
    });

    test("this values of sloppy and strict callees", () => {
        function sloppy() {
            return this;
        }
        function strict() {
            "use strict";
            return this;
        }
        const object = { sloppy, strict };
        function caller(value) {
            return [sloppy(), strict(), object.sloppy(), object.strict(), sloppy.call(value), strict.call(value)];
        }
        for (let i = 0; i < iterations; ++i) {
            const [sloppyThis, strictThis, sloppyMethodThis, strictMethodThis, boxedThis, primitiveThis] = caller(i);
            expect(sloppyThis).toBe(globalThis);
            expect(strictThis).toBeUndefined();
            expect(sloppyMethodThis).toBe(object);
            expect(strictMethodThis).toBe(object);
            expect(boxedThis).toBeInstanceOf(Number);
            expect(primitiveThis).toBe(i);
        }
    });

    test("exceptions thrown by callees reach the caller's handler", () => {
        function thrower(value) {
            if (value % 3 === 0) throw new Error(`thrown ${value}`);
            return value;
        }
        function caller(value) {
            try {
                return thrower(value) + 1;
            } catch (error) {
                return error.message;
            }
        }
        for (let i = 0; i < iterations; ++i) expect(caller(i)).toBe(i % 3 === 0 ? `thrown ${i}` : i + 1);
    });

    test("deep recursion works and running out of stack throws", () => {
        function depth(n) {
            if (n === 0) return 0;
            return 1 + depth(n - 1);
        }
        for (let i = 0; i < iterations; ++i) expect(depth(i)).toBe(i);
        expect(depth(3000)).toBe(3000);
        expect(() => depth(1e7)).toThrow();
        expect(depth(10)).toBe(10);
    });

    test("callees that change behavior or start throwing late", () => {
        function callee(value) {
            if (value > iterations) return value * 2;
            return value;
        }
        function caller(value) {
            return callee(value) + 1;
        }
        for (let i = 0; i < iterations; ++i) expect(caller(i)).toBe(i + 1);
        for (let i = 0; i < iterations; ++i) {
            const value = iterations + 1 + i;
            expect(caller(value)).toBe(value * 2 + 1);
        }
    });

    test("call sites that see other callees later", () => {
        function first(value) {
            return value + 1;
        }
        function second(value) {
            return value + 2;
        }
        function caller(f, value) {
            return f(value);
        }
        for (let i = 0; i < iterations; ++i) expect(caller(first, i)).toBe(i + 1);
        for (let i = 0; i < iterations; ++i) expect(caller(second, i)).toBe(i + 2);
        expect(caller(Math.abs, -5)).toBe(5);
        expect(caller(first.bind(null, 10), 0)).toBe(11);
    });

    test("stack traces show directly called frames", () => {
        function inner() {
            return new Error().stack;
        }
        function middle() {
            return inner();
        }
        function outer() {
            return middle();
        }
        for (let i = 0; i < iterations; ++i) {
            const names = outer()
                .split("\n")
                .slice(1)
                .map(line => line.trim().split(" ")[1]);
            expect(names.indexOf("inner")).toBeLessThan(names.indexOf("middle"));
            expect(names.indexOf("middle")).toBeLessThan(names.indexOf("outer"));
        }
    });

    test("call sites that see callees only the generic call can call later", () => {
        function target(value) {
            // NB: Too large to inline.
            let x = value;
            x = x + 1 - 1;
            x = x + 2 - 2;
            x = x + 3 - 3;
            x = x + 4 - 4;
            x = x + 5 - 5;
            x = x + 6 - 6;
            x = x + 7 - 7;
            x = x + 8 - 8;
            x = x + 9 - 9;
            x = x + 10 - 10;
            x = x + 11 - 11;
            x = x + 12 - 12;
            x = x + 13 - 13;
            x = x + 14 - 14;
            x = x + 15 - 15;
            x = x + 16 - 16;
            return x + 1;
        }
        function caller(f, value) {
            return f(value, value);
        }
        for (let i = 0; i < iterations; ++i) expect(caller(target, i)).toBe(i + 1);
        // NB: The generic call reads the call's operands from the frame.
        const proxy = new Proxy(function (value) {
            return value * 2;
        }, {});
        expect(caller(proxy, 3)).toBe(6);
        expect(() => caller(5, 5)).toThrowWithMessage(TypeError, "5 is not a function");
    });

    test("closures of one function, each with its own environment", () => {
        function makeCounter(start) {
            let count = start;
            return function (step) {
                count += step;
                return [this === undefined ? "undefined" : typeof this, count];
            };
        }
        function call(counter, step) {
            return counter(step);
        }
        for (let i = 0; i < iterations; ++i) {
            const counter = makeCounter(i * 100);
            expect(call(counter, 1)).toEqual(["object", i * 100 + 1]);
            expect(call(counter, 2)).toEqual(["object", i * 100 + 3]);
        }
        expect(call(x => x * 2, 21)).toBe(42);
        expect(() => call(makeCounter(0).bind(null), Symbol())).toThrow(TypeError);
    });

    test("recursive named function expressions made for each call", () => {
        function sumTree(tree) {
            let sum = 0;
            (function walk(node) {
                sum += node.value;
                for (const child of node.children) walk(child);
            })(tree);
            return sum;
        }
        const leaf = value => ({ value, children: [] });
        for (let i = 0; i < iterations; ++i) {
            const tree = { value: i, children: [leaf(1), { value: 2, children: [leaf(3), leaf(4)] }] };
            expect(sumTree(tree)).toBe(i + 10);
        }
        expect(() => sumTree({ value: 1, children: [null] })).toThrow(TypeError);
    });
});
