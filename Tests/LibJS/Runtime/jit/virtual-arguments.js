// JIT code reads arguments.length, arguments[i] and forwards
// f.apply(this, arguments) from the frame without creating the arguments
// object, and creates it when anything else may see it. These tests run every
// function many times, so that with a low JIT threshold they are compiled.

const iterations = 100;

describe("arguments objects in JIT code", () => {
    test("length and indexed reads", () => {
        function sum() {
            let s = 0;
            for (let i = 0; i < arguments.length; i++) s += arguments[i];
            return s;
        }
        function strictSum() {
            "use strict";
            let s = 0;
            for (let i = 0; i < arguments.length; i++) s += arguments[i];
            return s;
        }
        for (let i = 0; i < iterations; ++i) {
            expect(sum(i, 2, 3)).toBe(i + 5);
            expect(sum()).toBe(0);
            expect(strictSum(i, 1)).toBe(i + 1);
        }
    });

    test("reads past the passed arguments and odd indices", () => {
        function read(a, b, index) {
            return arguments[index];
        }
        for (let i = 0; i < iterations; ++i) expect(read(i, 1, 2)).toBe(2);
        expect(read(1, 2)).toBeUndefined();
        expect(read(1, 2, 7)).toBeUndefined();
        expect(read(1, 2, -1)).toBeUndefined();
        expect(read(1, 2, "length")).toBe(3);
        expect(read(1, 2, 1.5)).toBeUndefined();
        expect(read(5, 2, 0)).toBe(5);
    });

    test("exits with the arguments object live", () => {
        function exitsThenUses(flag) {
            const length = arguments.length;
            if (flag) {
                // Never ran while the function warmed up, so compiled code
                // exits here, and the interpreter returns the object.
                const property = flag.property;
                return arguments;
            }
            return length + arguments[0];
        }
        for (let i = 0; i < iterations; ++i) expect(exitsThenUses(0, 2, 3)).toBe(3);
        const object = exitsThenUses(1, 2, 3);
        expect(object.length).toBe(3);
        expect(Array.from(object)).toEqual([1, 2, 3]);
        expect(Object.prototype.toString.call(object)).toBe("[object Arguments]");
    });

    test("exceptions caught in the function that reads its arguments", () => {
        function catchesAndReads(value) {
            let result;
            try {
                result = value.missing.property;
            } catch {
                result = arguments.length + arguments[1];
            }
            return result;
        }
        for (let i = 0; i < iterations; ++i) {
            expect(catchesAndReads({ missing: { property: i } }, 1)).toBe(i);
            expect(catchesAndReads(undefined, 10)).toBe(12);
        }
    });

    test("forwarding with Function.prototype.apply", () => {
        const target = {
            base: 1,
            inner(a, b, c) {
                return this.base + a + b + c;
            },
        };
        function forward() {
            return target.inner.apply(this, arguments);
        }
        target.forward = forward;
        for (let i = 0; i < iterations; ++i) expect(target.forward(i, 2, 3)).toBe(i + 6);
        // Fewer arguments, another receiver, and a callee that is no function.
        expect(target.forward(1)).toBeNaN();
        expect(forward.call({ base: 10 }, 1, 2, 3)).toBe(16);
        const inner = target.inner;
        target.inner = Object.create(Function.prototype);
        expect(() => target.forward(1, 2, 3)).toThrowWithMessage(TypeError, "is not a function");
        target.inner = inner;
        expect(target.forward(1, 2, 3)).toBe(7);
    });

    test("forwarding with spread", () => {
        function sum(a, b, c) {
            return a + b + c;
        }
        function spread() {
            return sum(...arguments);
        }
        function spreadWithThis() {
            return this.sum(...arguments);
        }
        const object = { sum, value: 1 };
        for (let i = 0; i < iterations; ++i) {
            expect(spread(i, 2, 3)).toBe(i + 5);
            expect(spreadWithThis.call(object, i, 1, 1)).toBe(i + 2);
        }
        expect(spread(1)).toBeNaN();

        // Observable iteration, through a replaced %ArrayIteratorPrototype%.next.
        const iteratorPrototype = Object.getPrototypeOf([][Symbol.iterator]());
        const next = iteratorPrototype.next;
        let steps = 0;
        iteratorPrototype.next = function () {
            ++steps;
            return next.call(this);
        };
        try {
            expect(spread(1, 2, 3)).toBe(6);
            expect(steps).toBe(4);
        } finally {
            iteratorPrototype.next = next;
        }

        // Callees that are not functions throw after the spread.
        function spreadIntoUndefined() {
            const notAFunction = undefined;
            return notAFunction(...arguments);
        }
        for (let i = 0; i < iterations; ++i) expect(() => spreadIntoUndefined(i)).toThrow(TypeError);
    });

    test("arguments that escape", () => {
        function escapes() {
            return arguments;
        }
        function callee() {
            return arguments.callee;
        }
        function writesParameter(a) {
            a = 10;
            return arguments[0];
        }
        function strictWritesParameter(a) {
            "use strict";
            a = 10;
            return arguments[0];
        }
        function passesOn() {
            return Array.prototype.slice.call(arguments, 1);
        }
        for (let i = 0; i < iterations; ++i) {
            expect(escapes(i, 1).length).toBe(2);
            expect(callee()).toBe(callee);
            expect(writesParameter(i)).toBe(10);
            expect(strictWritesParameter(i)).toBe(i);
            expect(passesOn(1, 2, 3)).toEqual([2, 3]);
        }
    });

    test("loops over arguments that get compiled while running", () => {
        function sumFrom(start) {
            let sum = 0;
            for (let round = 0; round < 50; ++round) {
                for (let i = start; i < arguments.length; ++i) sum += arguments[i];
            }
            return sum;
        }
        function forwardInLoop(times) {
            let total = 0;
            for (let i = 0; i < times; ++i) total += sumFrom.apply(null, arguments);
            return total;
        }
        for (let i = 0; i < iterations; ++i) {
            expect(sumFrom(1, 2, 3, i)).toBe(50 * (5 + i));
            expect(forwardInLoop(2, 5, 6)).toBe(2 * 50 * 6);
        }
    });

    test("slow paths of inlined callees, and exceptions out of them", () => {
        function describe(value) {
            if (value === 3) throw new Error("three");
            return typeof value;
        }
        function caller() {
            let result;
            try {
                result = describe(arguments[0]) + arguments.length;
            } catch (error) {
                result = error.message + arguments.length + arguments[1];
            }
            return result;
        }
        for (let i = 0; i < iterations; ++i) {
            expect(caller(i % 4, "x")).toBe(i % 4 === 3 ? "three2x" : "number2");
            expect(caller("s")).toBe("string1");
        }
    });

    test("slices of arguments through Array.prototype.slice.call", () => {
        const slice = [].slice;
        function children(type) {
            return arguments.length > 1 ? slice.call(arguments, 1) : null;
        }
        function all() {
            return Array.prototype.slice.call(arguments);
        }
        function last(count) {
            return slice.call(arguments, -count);
        }
        function withStart(start) {
            return slice.call(arguments, start);
        }
        for (let i = 0; i < iterations; ++i) {
            expect(children("div", i, "x")).toEqual([i, "x"]);
            expect(children("div")).toBeNull();
            expect(all(i, 2)).toEqual([i, 2]);
            expect(all()).toEqual([]);
            expect(last(2, "a", "b")).toEqual(["a", "b"]);
            expect(withStart(10, 1)).toEqual([]);
            expect(withStart("1", 2)).toEqual([2]);
            expect(withStart(1.5, 2)).toEqual([2]);
            const array = children("a", 1, 2);
            expect(Array.isArray(array)).toBeTrue();
            array.push(3);
            expect(array).toEqual([1, 2, 3]);
        }
    });

    test("arguments of inlined callees", () => {
        function count() {
            return arguments.length;
        }
        function caller(i) {
            return count(i, i) + count();
        }
        for (let i = 0; i < iterations; ++i) expect(caller(i)).toBe(2);
    });
});
