// JIT code stores to the element at the length of a packed array inline, when
// nothing could observe the difference. These tests run every function many
// times, so that with a low JIT threshold they are compiled.

const iterations = 100;

function store(array, index, value) {
    array[index] = value;
}

function fill(count) {
    const array = [];
    for (let i = 0; i < count; ++i) store(array, array.length, i);
    return array;
}

describe("Appending stores in JIT code", () => {
    test("appending and growing", () => {
        for (let i = 0; i < iterations; ++i) {
            const array = fill(i);
            expect(array.length).toBe(i);
            for (let j = 0; j < i; ++j) expect(array[j]).toBe(j);
        }
        const array = new Array(4);
        array.length = 0;
        for (let i = 0; i < 8; ++i) store(array, i, `${i}`);
        expect(array).toEqual(["0", "1", "2", "3", "4", "5", "6", "7"]);
    });

    test("stores past the length leave holes", () => {
        for (let i = 0; i < iterations; ++i) fill(4);
        const array = fill(2);
        store(array, 3, "x");
        expect(array.length).toBe(4);
        expect(Object.hasOwn(array, 2)).toBeFalse();
        expect(array[3]).toBe("x");
        store(array, -1, "negative");
        expect(array.length).toBe(4);
        expect(array[-1]).toBe("negative");
    });

    test("arrays it cannot append to unobservably", () => {
        for (let i = 0; i < iterations; ++i) fill(4);

        const frozen = Object.freeze([1]);
        store(frozen, 1, 2);
        expect(frozen).toEqual([1]);

        const sealed = Object.seal([1]);
        store(sealed, 1, 2);
        expect(sealed).toEqual([1]);

        const nonExtensible = Object.preventExtensions([1]);
        store(nonExtensible, 1, 2);
        expect(nonExtensible).toEqual([1]);

        const fixedLength = [1];
        Object.defineProperty(fixedLength, "length", { writable: false });
        store(fixedLength, 1, 2);
        expect(fixedLength.length).toBe(1);
        expect(Object.hasOwn(fixedLength, 1)).toBeFalse();

        const arrayLike = { length: 1, 0: "a" };
        store(arrayLike, 1, "b");
        expect(arrayLike[1]).toBe("b");
        expect(arrayLike.length).toBe(1);

        class Subclass extends Array {}
        const subclass = new Subclass();
        store(subclass, 0, 5);
        expect(subclass.length).toBe(1);
        expect(subclass[0]).toBe(5);

        const proxied = [];
        let sets = 0;
        const proxy = new Proxy(proxied, {
            set(target, key, value) {
                ++sets;
                target[key] = value;
                return true;
            },
        });
        store(proxy, 0, 1);
        expect(sets).toBe(1);
        expect(proxied).toEqual([1]);
        store(proxied, 1, 2);
        expect(proxied).toEqual([1, 2]);
    });

    test("setters on the prototype chain", () => {
        for (let i = 0; i < iterations; ++i) fill(4);
        let arraySetterCalls = 0;
        Object.defineProperty(Array.prototype, 2, {
            set(value) {
                ++arraySetterCalls;
            },
            configurable: true,
        });
        try {
            const array = [0, 1];
            store(array, 2, 2);
            expect(arraySetterCalls).toBe(1);
            expect(Object.hasOwn(array, 2)).toBeFalse();
        } finally {
            delete Array.prototype[2];
        }

        let objectSetterCalls = 0;
        Object.defineProperty(Object.prototype, 1, {
            set(value) {
                ++objectSetterCalls;
            },
            configurable: true,
        });
        try {
            const array = [0];
            store(array, 1, 1);
            expect(objectSetterCalls).toBe(1);
            expect(array.length).toBe(1);
        } finally {
            delete Object.prototype[1];
        }

        const withOwnPrototype = [0];
        Object.setPrototypeOf(withOwnPrototype, {
            set 1(value) {
                withOwnPrototype.seen = value;
            },
        });
        store(withOwnPrototype, 1, "seen");
        expect(withOwnPrototype.seen).toBe("seen");
        expect(withOwnPrototype.length).toBe(1);

        expect(fill(4)).toEqual([0, 1, 2, 3]);
    });
});
