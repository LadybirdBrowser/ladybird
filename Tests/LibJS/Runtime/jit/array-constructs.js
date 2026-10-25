// JIT code allocates the arrays `new Array(...)` makes of no argument or of
// several arguments itself, and those of a length through the runtime.
// These tests run every function many times, so that with a low JIT
// threshold they are compiled.

const iterations = 100;

describe("constructs of Array in JIT code", () => {
    test("no arguments, a length, and several elements", () => {
        function empty() {
            return new Array();
        }
        function ofLength(n) {
            return new Array(n);
        }
        function ofElements(a, b, c) {
            return new Array(a, b, c);
        }
        for (let i = 0; i < iterations; ++i) {
            const array = empty();
            expect(Array.isArray(array)).toBeTrue();
            expect(Object.getPrototypeOf(array)).toBe(Array.prototype);
            expect(array.length).toBe(0);
            array.push(i);
            expect(array).toEqual([i]);
            expect(empty()).not.toBe(empty());

            const sized = ofLength(i % 5);
            expect(sized.length).toBe(i % 5);
            expect(0 in sized).toBeFalse();

            const elements = ofElements(i, "x", null);
            expect(elements).toEqual([i, "x", null]);
            expect(Object.getPrototypeOf(elements)).toBe(Array.prototype);
        }
    });

    test("single arguments that are no lengths", () => {
        function ofOne(value) {
            return new Array(value);
        }
        for (let i = 0; i < iterations; ++i) {
            expect(ofOne(3).length).toBe(3);
            expect(ofOne("3")).toEqual(["3"]);
            expect(ofOne(null)).toEqual([null]);
        }
        expect(() => ofOne(-1)).toThrow(RangeError);
        expect(() => ofOne(1.5)).toThrow(RangeError);
        expect(ofOne(2 ** 31).length).toBe(2 ** 31);
    });

    test("constructors other than Array", () => {
        class MyArray extends Array {}
        function construct(C) {
            return new C();
        }
        for (let i = 0; i < iterations; ++i) expect(Array.isArray(construct(Array))).toBeTrue();
        const mine = construct(MyArray);
        expect(mine).toBeInstanceOf(MyArray);
        expect(construct(Object)).not.toBeInstanceOf(Array);
    });

    test("a replaced global Array", () => {
        const OriginalArray = Array;
        function make() {
            return new Array();
        }
        for (let i = 0; i < iterations; ++i) expect(Array.isArray(make())).toBeTrue();
        try {
            globalThis.Array = function () {
                this.replaced = true;
            };
            expect(make().replaced).toBeTrue();
        } finally {
            globalThis.Array = OriginalArray;
        }
        expect(Array.isArray(make())).toBeTrue();
    });
});
