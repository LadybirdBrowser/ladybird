// JIT code calls the callees of call sites that saw more than one callee
// directly, reading what it needs from the function object at call time.
// These tests run every caller and callee many times, so that with a low JIT
// threshold they are compiled.

const iterations = 100;

describe("polymorphic calls from JIT code", () => {
    test("arguments, missing arguments and return values of many callees", () => {
        const callees = [
            (a, b) => [a, b],
            function (a) {
                return [a, arguments.length];
            },
            function (a, b, c, d) {
                return [a, b, c, d];
            },
            function () {
                return arguments.length;
            },
            function (a) {
                if (a < 0) return;
                return a * 2;
            },
            Math.max,
        ];
        function caller(f, i) {
            return [f(i), f(i, 1, 2, 3, 4)];
        }
        for (let i = 0; i < iterations; ++i) {
            expect(caller(callees[0], i)).toEqual([
                [i, undefined],
                [i, 1],
            ]);
            expect(caller(callees[1], i)).toEqual([
                [i, 1],
                [i, 5],
            ]);
            expect(caller(callees[2], i)).toEqual([
                [i, undefined, undefined, undefined],
                [i, 1, 2, 3],
            ]);
            expect(caller(callees[3], i)).toEqual([1, 5]);
            expect(caller(callees[4], i)).toEqual([i * 2, i * 2]);
            expect(caller(callees[4], -1)).toEqual([undefined, undefined]);
            expect(caller(callees[5], i)).toEqual([i, Math.max(i, 4)]);
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
        function sloppyWithoutThis(value) {
            return value;
        }
        const object = { sloppy, strict };
        function caller(f, receiver, value) {
            return [f(value), f.call(receiver, value), receiver.sloppy(), receiver.strict()];
        }
        for (let i = 0; i < iterations; ++i) {
            expect(caller(sloppy, object, i)).toEqual([globalThis, object, object, object]);
            expect(caller(strict, object, i)).toEqual([undefined, object, object, object]);
            expect(caller(sloppyWithoutThis, object, i)).toEqual([i, i, object, object]);
            const [, boxed] = caller(sloppy, Object.assign(Object.create(object), {}), i);
            expect(boxed).toBeInstanceOf(Object);
        }
        expect(sloppy.call(5)).toBeInstanceOf(Number);
        expect(strict.call(5)).toBe(5);
    });

    test("closures of one function and of many", () => {
        function make(n) {
            return value => value + n;
        }
        function makeCounter() {
            let count = 0;
            return () => ++count;
        }
        const adders = [make(1), make(2), make(3), make(4), make(5)];
        const counters = [makeCounter(), makeCounter()];
        function caller(f, value) {
            return f(value);
        }
        for (let i = 0; i < iterations; ++i) {
            for (let k = 0; k < adders.length; ++k) expect(caller(adders[k], i)).toBe(i + k + 1);
            expect(caller(counters[i % 2], 0)).toBe(Math.floor(i / 2) + 1);
        }
    });

    test("exceptions thrown by callees reach the caller's handler", () => {
        const callees = [
            value => {
                if (value % 3 === 0) throw new Error(`first ${value}`);
                return value;
            },
            value => {
                if (value % 5 === 0) throw new Error(`second ${value}`);
                return -value;
            },
        ];
        function caller(f, value) {
            try {
                return f(value);
            } catch (error) {
                return error.message;
            }
        }
        for (let i = 0; i < iterations; ++i) {
            expect(caller(callees[0], i)).toBe(i % 3 === 0 ? `first ${i}` : i);
            expect(caller(callees[1], i)).toBe(i % 5 === 0 ? `second ${i}` : -i);
        }
    });

    test("deep recursion through a polymorphic call site", () => {
        const table = {};
        function call(name, n) {
            return table[name](n);
        }
        table.even = n => (n === 0 ? true : call("odd", n - 1));
        table.odd = n => (n === 0 ? false : call("even", n - 1));
        for (let i = 0; i < iterations; ++i) expect(call(i % 2 ? "odd" : "even", i)).toBeTrue();
        expect(call("even", 3000)).toBeTrue();
        expect(() => call("even", 1e7)).toThrow();
        expect(call("odd", 11)).toBeTrue();
    });

    test("Function.prototype.call with ECMAScript and native targets", () => {
        const hasOwn = Object.prototype.hasOwnProperty;
        const toString = Object.prototype.toString;
        function getX() {
            return this.x;
        }
        function getY() {
            "use strict";
            return this === undefined ? "none" : this.y;
        }
        function caller(f, receiver, key) {
            return [f.call(receiver, key), hasOwn.call(receiver, key), toString.call(receiver)];
        }
        for (let i = 0; i < iterations; ++i) {
            const object = { x: i, y: -i };
            expect(caller(getX, object, "x")).toEqual([i, true, "[object Object]"]);
            expect(caller(getY, object, "z")).toEqual([-i, false, "[object Object]"]);
            expect(caller(hasOwn, object, "x")).toEqual([true, true, "[object Object]"]);
        }
        function noReceiver(f) {
            return f.call();
        }
        for (let i = 0; i < iterations; ++i) {
            expect(noReceiver(getY)).toBe("none");
            expect(noReceiver(() => i)).toBe(i);
        }
        expect(() => noReceiver(getX)).not.toThrow();
        expect(() => caller(null, {}, "x")).toThrow(TypeError);
    });

    test("native callees that throw", () => {
        const callees = [JSON.parse, Math.abs, String.prototype.toUpperCase];
        function caller(f, value) {
            try {
                return f(value);
            } catch (error) {
                return error.constructor.name;
            }
        }
        function callThrough(f, receiver, value) {
            try {
                return f.call(receiver, value);
            } catch (error) {
                return error.constructor.name;
            }
        }
        const hasOwn = Object.prototype.hasOwnProperty;
        for (let i = 0; i < iterations; ++i) {
            expect(caller(callees[0], "[1]")).toEqual([1]);
            expect(caller(callees[0], "{")).toBe("SyntaxError");
            expect(caller(callees[1], -i)).toBe(i);
            expect(caller(callees[2], "a")).toBe("TypeError");
            expect(callThrough(hasOwn, { x: 1 }, "x")).toBeTrue();
            expect(callThrough(hasOwn, null, "x")).toBe("TypeError");
            expect(callThrough(String.prototype.toUpperCase, "a")).toBe("A");
        }
    });

    test("callees that need a function environment or this resolution", () => {
        function counter(start) {
            let count = start;
            return function (step) {
                count += step;
                const read = () => count;
                return read();
            };
        }
        function sloppyThis() {
            const arrow = () => this;
            return arrow();
        }
        function strictThis() {
            "use strict";
            const arrow = () => this;
            return arrow();
        }
        function mapped(a, b) {
            arguments[0] = b;
            const get = () => a;
            return get();
        }
        function withDefault(a, unused, b = () => a) {
            return b();
        }
        const object = { sloppyThis, strictThis };
        const callees = [counter(0), counter(100), sloppyThis, strictThis, mapped, withDefault];
        function caller(f, receiver, value) {
            return f.call(receiver, value, value + 1);
        }
        function plain(f, value) {
            return f(value, value + 1);
        }
        for (let i = 0; i < iterations; ++i) {
            expect(caller(callees[0], undefined, 1)).toBe(i + 1);
            expect(plain(callees[1], 2)).toBe(100 + 2 * (i + 1));
            expect(caller(callees[2], object, i)).toBe(object);
            expect(plain(callees[2], i)).toBe(globalThis);
            expect(caller(callees[2], i, i)).toBeInstanceOf(Number);
            expect(caller(callees[3], i, i)).toBe(i);
            expect(plain(callees[3], i)).toBeUndefined();
            expect(plain(callees[4], i)).toBe(i + 1);
            expect(plain(callees[5], i)).toBe(i);
            expect(object.sloppyThis()).toBe(object);
        }
    });

    test("arrow functions keep their this at polymorphic call sites", () => {
        const outer = {
            make() {
                return () => this;
            },
        };
        const arrows = [outer.make(), outer.make.call(42), () => "other"];
        function caller(f, receiver) {
            return f.call(receiver);
        }
        for (let i = 0; i < iterations; ++i) {
            expect(caller(arrows[0], {})).toBe(outer);
            expect(caller(arrows[1], {}) instanceof Number).toBeTrue();
            expect(caller(arrows[2], {})).toBe("other");
        }
    });

    test("exceptions from callees with environments", () => {
        function make(limit) {
            let calls = 0;
            return value => {
                if (++calls > limit) throw new Error(`limit ${limit}`);
                return value;
            };
        }
        const callees = [make(1e9), make(50)];
        function caller(f, value) {
            try {
                return f(value);
            } catch (error) {
                return error.message;
            }
        }
        for (let i = 0; i < iterations; ++i) {
            expect(caller(callees[0], i)).toBe(i);
            expect(caller(callees[1], i)).toBe(i < 50 ? i : "limit 50");
        }
    });

    test("callees that are not functions throw", () => {
        function caller(f) {
            return f();
        }
        for (let i = 0; i < iterations; ++i) {
            expect(caller(() => 1)).toBe(1);
            expect(
                caller(function () {
                    return 2;
                })
            ).toBe(2);
        }
        expect(() => caller(1)).toThrow(TypeError);
        expect(() => caller({})).toThrow(TypeError);
        expect(caller(() => 3)).toBe(3);
    });

    test("stack traces show polymorphically called frames", () => {
        function inner() {
            return new Error().stack;
        }
        function other() {
            return "";
        }
        function middle(f) {
            return f();
        }
        for (let i = 0; i < iterations; ++i) {
            middle(other);
            const names = middle(inner)
                .split("\n")
                .slice(1)
                .map(line => line.trim().split(" ")[1]);
            expect(names.indexOf("inner")).toBeLessThan(names.indexOf("middle"));
        }
    });
});
