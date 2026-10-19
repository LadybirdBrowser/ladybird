// JIT code stores into the holes of holey arrays inline, when nothing could
// observe the difference. These tests run every function many times, so that
// with a low JIT threshold they are compiled.

const iterations = 100;

function store(array, index, value) {
    array[index] = value;
}

function strictStore(array, index, value) {
    "use strict";
    array[index] = value;
}

function fill(values) {
    const array = new Array(values.length);
    for (let i = 0; i < values.length; ++i) store(array, i, values[i]);
    return array;
}

function warmUp() {
    const values = Array.from({ length: 16 }, (_, i) => i);
    for (let i = 0; i < iterations; ++i) {
        fill(values);
        const array = new Array(values.length);
        for (let j = 0; j < values.length; ++j) strictStore(array, j, j);
    }
}

// An array with holes inside the capacity of its storage, so that stores into
// them do not need to grow it.
function holeyArray(length) {
    const array = new Array(length);
    array[0] = 0;
    return array;
}

describe("Hole-filling stores in JIT code", () => {
    test("filling every hole", () => {
        const kinds = [i => i, i => `${i}`, i => ({ i }), () => undefined];
        for (let count = 0; count <= iterations; ++count) {
            for (const kind of kinds) {
                const values = Array.from({ length: count }, (_, i) => kind(i));
                const array = fill(values);
                expect(array.length).toBe(count);
                for (let i = 0; i < count; ++i) {
                    expect(Object.hasOwn(array, i)).toBeTrue();
                    expect(array[i]).toBe(values[i]);
                }
                expect(array).toEqual(values);
                expect([...array]).toEqual(values);
                expect(Object.keys(array)).toEqual(Object.keys(values));
            }
        }
    });

    test("overwriting elements of holey arrays", () => {
        warmUp();
        const array = holeyArray(8);
        store(array, 2, "a");
        store(array, 2, "b");
        strictStore(array, 3, "c");
        strictStore(array, 3, "d");
        expect(array[2]).toBe("b");
        expect(array[3]).toBe("d");
        expect(Object.keys(array)).toEqual(["0", "2", "3"]);
    });

    test("filling some holes leaves the others", () => {
        warmUp();
        for (let count = 0; count <= 40; ++count) {
            const array = new Array(count);
            for (let i = 0; i < count; i += 2) store(array, i, i);
            expect(array.length).toBe(count);
            for (let i = 0; i < count; ++i) {
                const filled = i % 2 === 0;
                expect(Object.hasOwn(array, i)).toBe(filled);
                expect(i in array).toBe(filled);
                expect(array[i]).toBe(filled ? i : undefined);
            }
            expect(Object.keys(array).length).toBe(Math.ceil(count / 2));
        }
    });

    test("filling in reverse and the last index first", () => {
        warmUp();
        const expected = Array.from({ length: 20 }, (_, i) => i);

        const reversed = new Array(20);
        for (let i = 19; i >= 0; --i) store(reversed, i, i);
        expect(reversed).toEqual(expected);

        const lastFirst = holeyArray(20);
        store(lastFirst, 19, 19);
        for (let i = 1; i < 19; ++i) store(lastFirst, i, i);
        expect(lastFirst).toEqual(expected);

        const lastAtEnd = holeyArray(20);
        for (let i = 1; i < 20; ++i) store(lastAtEnd, i, i);
        expect(lastAtEnd).toEqual(expected);

        for (const array of [reversed, lastFirst, lastAtEnd]) {
            expect(array[19]).toBe(19);
            expect(array.indexOf(7)).toBe(7);
            expect(array.indexOf(20)).toBe(-1);
            expect(array.push(20)).toBe(21);
            expect([...array]).toEqual([...expected, 20]);
            expect(Object.keys(array).length).toBe(21);
        }
    });

    test("negative keys of arrays longer than 2^31", () => {
        warmUp();
        const array = new Array(2 ** 32 - 1);
        store(array, -5, "negative");
        strictStore(array, -6, "also negative");
        expect(array[-5]).toBe("negative");
        expect(array[-6]).toBe("also negative");
        expect(array[2 ** 32 - 5]).toBeUndefined();
        expect(array.length).toBe(2 ** 32 - 1);
        expect(Object.keys(array)).toEqual(["-5", "-6"]);
    });

    test("arrays that cannot get new elements", () => {
        warmUp();
        const array = Object.preventExtensions(holeyArray(8));
        store(array, 3, "x");
        expect(Object.hasOwn(array, 3)).toBeFalse();
        expect(array.length).toBe(8);
        expect(() => strictStore(array, 3, "x")).toThrow(TypeError);
        expect(Object.hasOwn(array, 3)).toBeFalse();
        store(array, 0, "existing");
        expect(array[0]).toBe("existing");

        const sealed = Object.seal(holeyArray(8));
        store(sealed, 3, "x");
        expect(Object.hasOwn(sealed, 3)).toBeFalse();
        expect(() => strictStore(sealed, 3, "x")).toThrow(TypeError);
    });

    test("arrays with other prototypes", () => {
        warmUp();
        class Subclass extends Array {
            set 3(value) {
                this.seen = value;
            }
        }
        const subclass = new Subclass(8);
        subclass[0] = 0;
        store(subclass, 3, "x");
        expect(subclass.seen).toBe("x");
        expect(Object.hasOwn(subclass, 3)).toBeFalse();
        store(subclass, 4, "y");
        expect(subclass[4]).toBe("y");

        const calls = [];
        const withOwnPrototype = holeyArray(8);
        Object.setPrototypeOf(withOwnPrototype, {
            set 2(value) {
                calls.push([this, value]);
            },
        });
        store(withOwnPrototype, 2, "x");
        strictStore(withOwnPrototype, 2, "y");
        expect(calls).toEqual([
            [withOwnPrototype, "x"],
            [withOwnPrototype, "y"],
        ]);
        expect(Object.hasOwn(withOwnPrototype, 2)).toBeFalse();
        store(withOwnPrototype, 3, "z");
        expect(Object.hasOwn(withOwnPrototype, 3)).toBeTrue();
    });

    test("proxy targets and other objects", () => {
        warmUp();
        const target = holeyArray(8);
        let sets = 0;
        const proxy = new Proxy(target, {
            set(target, key, value) {
                ++sets;
                target[key] = value;
                return true;
            },
        });
        store(target, 3, "x");
        expect(target[3]).toBe("x");
        expect(sets).toBe(0);
        store(proxy, 4, "y");
        expect(target[4]).toBe("y");
        expect(sets).toBe(1);
        expect(Object.keys(target)).toEqual(["0", "3", "4"]);

        const arrayLike = { length: 8, 0: 0, 5: 5 };
        store(arrayLike, 3, "x");
        expect(arrayLike[3]).toBe("x");
        expect(arrayLike.length).toBe(8);
        expect(Object.keys(arrayLike)).toEqual(["0", "3", "5", "length"]);
    });
});
