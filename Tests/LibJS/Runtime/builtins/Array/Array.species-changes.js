// ArraySpeciesCreate skips reading constructor and @@species for arrays of the realm while they are unchanged. These
// tests change them after methods that create arrays ran.

class Derived extends Array {}

function createArrays() {
    const array = [1, 2, 3];
    for (let i = 0; i < 10; ++i) {
        expect(array.map(x => x)).toEqual([1, 2, 3]);
        expect(array.filter(() => true)).toEqual([1, 2, 3]);
        expect(array.slice(1)).toEqual([2, 3]);
        expect(array.concat([4])).toEqual([1, 2, 3, 4]);
        expect(array.splice(0, 0)).toEqual([]);
    }
    return array;
}

function expectDerived(array) {
    expect(array.map(x => x)).toBeInstanceOf(Derived);
    expect(array.filter(() => true)).toBeInstanceOf(Derived);
    expect(array.slice(1)).toBeInstanceOf(Derived);
    expect(array.concat([4])).toBeInstanceOf(Derived);
    expect(array.splice(0, 0)).toBeInstanceOf(Derived);
}

test("a replaced Array.prototype.constructor is used", () => {
    const array = createArrays();
    const original = Array.prototype.constructor;
    Array.prototype.constructor = Derived;
    try {
        expectDerived(array);
    } finally {
        Array.prototype.constructor = original;
    }
    expect(array.slice(1)).not.toBeInstanceOf(Derived);
});

test("a replaced Array[@@species] getter is used", () => {
    const array = createArrays();
    const original = Object.getOwnPropertyDescriptor(Array, Symbol.species);
    Object.defineProperty(Array, Symbol.species, { get: () => Derived, configurable: true });
    try {
        expectDerived(array);
    } finally {
        Object.defineProperty(Array, Symbol.species, original);
    }
    expect(array.slice(1)).not.toBeInstanceOf(Derived);
});

test("an own constructor of the array is used", () => {
    const array = createArrays();
    array.constructor = Derived;
    expectDerived(array);
});

test("a replaced Array.prototype.constructor is used after unrelated properties changed", () => {
    createArrays();
    for (const object of [Array.prototype, Array, Object.prototype]) {
        object.unrelated = 1;
        createArrays();
        delete object.unrelated;
        createArrays();
    }
    const array = createArrays();
    const original = Array.prototype.constructor;
    Array.prototype.constructor = Derived;
    try {
        expectDerived(array);
    } finally {
        Array.prototype.constructor = original;
    }
});
