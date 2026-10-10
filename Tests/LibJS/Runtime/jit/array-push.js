// JIT code appends to arrays with Array.prototype.push inline, when nothing
// could observe the difference. These tests run every function many times,
// so that with a low JIT threshold they are compiled.

const iterations = 100;

function push(array, value) {
    return array.push(value);
}

function fill(count) {
    const array = [];
    for (let i = 0; i < count; ++i) push(array, i);
    return array;
}

describe("Array.prototype.push in JIT code", () => {
    test("appending and growing", () => {
        for (let i = 0; i < iterations; ++i) {
            const array = fill(i);
            expect(array.length).toBe(i);
            for (let j = 0; j < i; ++j) expect(array[j]).toBe(j);
        }
        const array = [1, 2];
        expect(push(array, "three")).toBe(3);
        expect(array).toEqual([1, 2, "three"]);
    });

    test("arrays it cannot append to unobservably", () => {
        for (let i = 0; i < iterations; ++i) fill(4);

        const frozen = Object.freeze([1]);
        expect(() => push(frozen, 2)).toThrow(TypeError);
        expect(frozen).toEqual([1]);

        const fixedLength = [1];
        Object.defineProperty(fixedLength, "length", { writable: false });
        expect(() => push(fixedLength, 2)).toThrow(TypeError);
        expect(fixedLength.length).toBe(1);

        const holey = [1, , 3];
        expect(push(holey, 4)).toBe(4);
        expect(holey[3]).toBe(4);

        const arrayLike = { length: 2, push: Array.prototype.push };
        expect(push(arrayLike, "x")).toBe(3);
        expect(arrayLike[2]).toBe("x");
        expect(arrayLike.length).toBe(3);

        class Subclass extends Array {}
        const subclass = new Subclass();
        expect(push(subclass, 5)).toBe(1);
        expect(subclass[0]).toBe(5);

        const proxied = new Proxy([], {});
        expect(push(proxied, 1)).toBe(1);
        expect(proxied[0]).toBe(1);
    });

    test("setters on the prototype chain", () => {
        for (let i = 0; i < iterations; ++i) fill(4);
        let setterCalls = 0;
        Object.defineProperty(Array.prototype, 2, {
            set(value) {
                ++setterCalls;
            },
            configurable: true,
        });
        try {
            const array = [0, 1];
            expect(push(array, 2)).toBe(3);
            expect(setterCalls).toBe(1);
            expect(Object.hasOwn(array, 2)).toBeFalse();
        } finally {
            delete Array.prototype[2];
        }
        expect(fill(4)).toEqual([0, 1, 2, 3]);
    });

    test("a push that is not of Array.prototype.push", () => {
        const array = [];
        array.push = function (value) {
            return "own push " + value;
        };
        expect(push(array, 1)).toBe("own push 1");
        expect(array.length).toBe(0);
    });
});
