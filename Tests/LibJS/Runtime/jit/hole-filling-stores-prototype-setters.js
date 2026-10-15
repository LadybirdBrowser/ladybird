// JIT code stores into the holes of holey arrays inline only where their
// prototype chain has no indexed properties. Each test compiles the stores
// first, and then gives %Array.prototype% or %Object.prototype% an indexed
// property. Indexed properties of %Object.prototype% stay in effect after they
// are deleted, so that test comes last, and this file is of its own.

const iterations = 100;

function store(array, index, value) {
    array[index] = value;
}

function strictStore(array, index, value) {
    "use strict";
    array[index] = value;
}

function warmUp() {
    for (let i = 0; i < iterations; ++i) {
        const array = new Array(16);
        for (let j = 0; j < 16; ++j) store(array, j, j);
        const strictArray = new Array(16);
        for (let j = 0; j < 16; ++j) strictStore(strictArray, j, j);
    }
}

// An array with holes inside the capacity of its storage, so that stores into
// them do not need to grow it.
function holeyArray(length) {
    const array = new Array(length);
    array[0] = 0;
    return array;
}

function removeArrayPrototypeElement(index) {
    delete Array.prototype[index];
    Array.prototype.length = 0;
}

describe("Hole-filling stores in JIT code with indexed properties on prototypes", () => {
    test("a setter on Array.prototype", () => {
        warmUp();
        const calls = [];
        Object.defineProperty(Array.prototype, 2, {
            set(value) {
                calls.push([this, value]);
            },
            configurable: true,
        });
        try {
            const array = holeyArray(8);
            store(array, 2, "x");
            strictStore(array, 2, "y");
            expect(calls).toEqual([
                [array, "x"],
                [array, "y"],
            ]);
            expect(Object.hasOwn(array, 2)).toBeFalse();
            store(array, 3, "z");
            expect(array[3]).toBe("z");
        } finally {
            removeArrayPrototypeElement(2);
        }
        const array = holeyArray(8);
        store(array, 2, "x");
        expect(array[2]).toBe("x");
        expect(calls.length).toBe(2);
    });

    test("a read-only element of Array.prototype", () => {
        warmUp();
        Object.defineProperty(Array.prototype, 3, { value: "read-only", configurable: true });
        try {
            const array = holeyArray(8);
            store(array, 3, "x");
            expect(Object.hasOwn(array, 3)).toBeFalse();
            expect(array[3]).toBe("read-only");
            expect(() => strictStore(array, 3, "x")).toThrow(TypeError);
            expect(Object.hasOwn(array, 3)).toBeFalse();
        } finally {
            removeArrayPrototypeElement(3);
        }
        const array = holeyArray(8);
        store(array, 3, "x");
        expect(array[3]).toBe("x");
    });

    test("a setter on Object.prototype", () => {
        warmUp();
        const calls = [];
        Object.defineProperty(Object.prototype, 4, {
            set(value) {
                calls.push([this, value]);
            },
            configurable: true,
        });
        try {
            const array = holeyArray(8);
            store(array, 4, "x");
            strictStore(array, 4, "y");
            expect(calls).toEqual([
                [array, "x"],
                [array, "y"],
            ]);
            expect(Object.hasOwn(array, 4)).toBeFalse();
        } finally {
            delete Object.prototype[4];
        }
        const array = holeyArray(8);
        store(array, 4, "x");
        expect(array[4]).toBe("x");
        expect(calls.length).toBe(2);
    });
});
