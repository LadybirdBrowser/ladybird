// JIT code inlines functions called through Function.prototype.call,
// Function.prototype.apply with the caller's arguments, and bound functions,
// as the interpreter saw them forward their calls. These tests run every
// function many times, so that with a low JIT threshold they are compiled.

const iterations = 100;

describe("calls forwarded to inlined functions", () => {
    test("Function.prototype.call", () => {
        const object = {
            base: 1,
            add(a, b) {
                return this.base + a + b;
            },
        };
        function caller(i) {
            return object.add.call(object, i, 2);
        }
        function withoutArguments() {
            return object.add.call();
        }
        for (let i = 0; i < iterations; ++i) {
            expect(caller(i)).toBe(i + 3);
            expect(withoutArguments()).toBeNaN();
        }
        // Another function, another receiver and a primitive receiver.
        const add = object.add;
        object.add = function (a, b) {
            return a * b;
        };
        expect(caller(5)).toBe(10);
        object.add = add;
        expect(caller.call(null, 5)).toBe(8);
        expect(add.call(3, 1, 1)).toBeNaN();
    });

    test("Function.prototype.apply with the caller's arguments", () => {
        const target = {
            base: 1,
            inner(a, b, c) {
                return this.base + a + b + c;
            },
        };
        function forward() {
            return target.inner.apply(target, arguments);
        }
        for (let i = 0; i < iterations; ++i) expect(forward(i, 2, 3)).toBe(i + 6);
        // Other argument counts than the ones seen.
        expect(forward(1, 2)).toBeNaN();
        expect(forward(1, 2, 3, 4)).toBe(7);
        expect(forward()).toBeNaN();
        for (let i = 0; i < iterations; ++i) expect(forward(i, 2, 3)).toBe(i + 6);
    });

    test("bound functions", () => {
        const object = { base: 10 };
        function add(a, b, c) {
            return this.base + a + b + c;
        }
        const bound = add.bind(object, 1);
        function caller(i) {
            return bound(i, 2);
        }
        for (let i = 0; i < iterations; ++i) expect(caller(i)).toBe(i + 13);
        const strictBound = function (a) {
            "use strict";
            return [this, a];
        }.bind(5, 6);
        function strictCaller() {
            return strictBound();
        }
        for (let i = 0; i < iterations; ++i) expect(strictCaller()).toEqual([5, 6]);
    });

    test("exits in forwarded callees", () => {
        const target = {
            inner(a, b, c) {
                if (a > 1000) {
                    // Never ran while warming up.
                    return [a, b, c, this === target];
                }
                return a + b + c;
            },
        };
        function forward() {
            return target.inner.apply(target, arguments);
        }
        function viaCall(a, b) {
            return target.inner.call(target, a, b, 3);
        }
        for (let i = 0; i < iterations; ++i) {
            expect(forward(i, 2, 3)).toBe(i + 5);
            expect(viaCall(i, 2)).toBe(i + 5);
        }
        expect(forward(2000, 2, 3)).toEqual([2000, 2, 3, true]);
        expect(viaCall(2000, 2)).toEqual([2000, 2, 3, true]);
    });

    test("exceptions and stack traces of forwarded calls", () => {
        const object = {
            check(value) {
                if (value % 5 === 4) throw new Error(`bad ${value}`);
                return value;
            },
        };
        function caller(i) {
            try {
                return object.check.call(object, i);
            } catch (error) {
                return error.message;
            }
        }
        for (let i = 0; i < iterations; ++i) expect(caller(i)).toBe(i % 5 === 4 ? `bad ${i}` : i);
    });
});
