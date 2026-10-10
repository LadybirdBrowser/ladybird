// Calls of the Array.prototype iteration builtins from hot functions, which the JIT inlines, with what makes the
// inlined code exit to the interpreter in the middle of a loop.

const ROUNDS = 300;

function sumWithForEach(array) {
    let sum = 0;
    array.forEach(value => {
        sum += value;
    });
    return sum;
}

function doubled(array) {
    return array.map(value => value * 2);
}

function odd(array) {
    return array.filter(value => value % 2 === 1);
}

test("results stay right while the callers get hot", () => {
    for (let i = 0; i < ROUNDS; ++i) {
        expect(sumWithForEach([1, 2, 3, i])).toBe(6 + i);
        expect(doubled([1, 2, i])).toEqual([2, 4, 2 * i]);
        expect(odd([1, 2, 3, i])).toEqual(i % 2 === 1 ? [1, 3, i] : [1, 3]);
    }
});

test("other values and receivers at a hot site", () => {
    for (let i = 0; i < ROUNDS; ++i) sumWithForEach([1, 2, 3]);
    expect(sumWithForEach([1.5, 2.5])).toBe(4);
    expect(sumWithForEach(["a", "b"])).toBe("0ab");
    expect(sumWithForEach({ length: 2, 0: 1, 1: 2, forEach: Array.prototype.forEach })).toBe(3);
    expect(sumWithForEach([1, , 3])).toBe(4);
    const withPrototypeElement = [1, , 3];
    Object.setPrototypeOf(withPrototypeElement, Object.create(Array.prototype, { 1: { value: 10 } }));
    expect(sumWithForEach(withPrototypeElement)).toBe(14);
    expect(sumWithForEach(new Proxy([4, 5], {}))).toBe(9);
    expect(doubled({ length: 1, 0: 4, map: Array.prototype.map })).toEqual([8]);
});

test("callbacks that change the array in the middle of the loop", () => {
    function shrinkAndGrow(array) {
        const seen = [];
        array.forEach((value, index) => {
            seen.push(value);
            if (index === 1) {
                array.length = 3;
                array.push(100);
            }
        });
        return seen;
    }
    for (let i = 0; i < ROUNDS; ++i) expect(shrinkAndGrow([1, 2])).toEqual([1, 2]);
    expect(shrinkAndGrow([1, 2, 3, 4, 5])).toEqual([1, 2, 3, 100]);
    expect(shrinkAndGrow([1, 2, 3])).toEqual([1, 2, 3]);
});

test("exceptions thrown by callbacks of inlined builtins", () => {
    function throwsAt(array, bad) {
        return array.map(value => {
            if (value === bad) throw new Error(`bad ${value}`);
            return value + 1;
        });
    }
    for (let i = 0; i < ROUNDS; ++i) expect(throwsAt([1, 2, 3], -1)).toEqual([2, 3, 4]);
    expect(() => throwsAt([1, 2, 3], 2)).toThrowWithMessage(Error, "bad 2");
    expect(() => throwsAt([1, 2, 3], 2)).toThrowWithMessage(Error, "bad 2");
    expect(throwsAt([5], 2)).toEqual([6]);
});

test("stack traces from callbacks of inlined builtins show the builtin", () => {
    function stackFromFilter(array) {
        let stack = null;
        array.filter(() => {
            stack = new Error().stack;
            return true;
        });
        return stack;
    }
    for (let i = 0; i < ROUNDS; ++i) stackFromFilter([1]);
    const stack = stackFromFilter([1]);
    expect(stack.includes("at filter\n")).toBeTrue();
    expect(stack.includes("BuiltinFile")).toBeFalse();
});

test("reduce with and without an initial value at hot sites", () => {
    function sum(array) {
        return array.reduce((a, b) => a + b);
    }
    function sumFrom(array, initial) {
        return array.reduce((a, b) => a + b, initial);
    }
    for (let i = 0; i < ROUNDS; ++i) {
        expect(sum([1, 2, i])).toBe(3 + i);
        expect(sumFrom([1, 2], i)).toBe(3 + i);
    }
    expect(() => sum([])).toThrowWithMessage(TypeError, "Reduce of empty array with no initial value");
    expect(sumFrom([], undefined)).toBeUndefined();
});

test("thisArg and the replaced builtins", () => {
    function withThis(array, thisArg) {
        return array.some(function (value) {
            "use strict";
            return this === thisArg && value > 1;
        }, thisArg);
    }
    const thisArg = {};
    for (let i = 0; i < ROUNDS; ++i) expect(withThis([1, 2], thisArg)).toBeTrue();
    expect(withThis([1, 2], 42)).toBeTrue();
    const originalSome = Array.prototype.some;
    Array.prototype.some = function () {
        return "replaced";
    };
    try {
        expect(withThis([1, 2], thisArg)).toBe("replaced");
    } finally {
        Array.prototype.some = originalSome;
    }
    expect(withThis([1, 2], thisArg)).toBeTrue();
});

test("arrays that map and filter create at hot sites", () => {
    function squares(array) {
        return array.map(value => value * value);
    }
    function evens(array) {
        return array.filter(value => value % 2 === 0);
    }
    for (let i = 0; i < ROUNDS; ++i) {
        expect(squares([1, 2, 3])).toEqual([1, 4, 9]);
        expect(evens([1, 2, 3, 4])).toEqual([2, 4]);
    }
    const holey = squares([1, , 3]);
    expect(holey.length).toBe(3);
    expect(1 in holey).toBeFalse();
    expect(holey[2]).toBe(9);
    const many = [];
    for (let i = 0; i < 1000; ++i) many.push(i);
    expect(evens(many).length).toBe(500);
    expect(squares(many)[999]).toBe(998001);

    class Frozen extends Array {
        static get [Symbol.species]() {
            return function () {
                return Object.freeze([]);
            };
        }
    }
    const frozenSpecies = Frozen.from([1, 2]);
    expect(() => squares(frozenSpecies)).toThrow(TypeError);
    expect(() => evens(frozenSpecies)).toThrow(TypeError);
});

test("callbacks that hot callers pass on to inlined builtins", () => {
    function isEven(value) {
        return value % 2 === 0;
    }
    function makeAbove(limit) {
        return value => value > limit;
    }
    function countMatching(array, predicate) {
        return array.filter(predicate).length;
    }
    for (let i = 0; i < ROUNDS; ++i) {
        expect(countMatching([1, 2, 3, 4], isEven)).toBe(2);
        expect(countMatching([1, 2, 3, 4], makeAbove(i % 4))).toBe(4 - (i % 4));
    }
    expect(countMatching([1.5, 2, 3.5], makeAbove(1.75))).toBe(2);
    expect(countMatching(["a", "b"], value => value === "b")).toBe(1);
    expect(countMatching([1, 2], Boolean)).toBe(2);
    expect(() => countMatching([1], 42)).toThrowWithMessage(TypeError, "42 is not a function");
    expect(countMatching([1, 2, 3, 4], isEven)).toBe(2);
    expect(countMatching([1, 2, 3, 4], makeAbove(2))).toBe(2);
});
