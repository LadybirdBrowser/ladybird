const originalIteratorMethod = Array.prototype[Symbol.iterator];

function collect(array) {
    const values = [];
    for (const value of array) values.push(value);
    return values;
}

function compileIteration() {
    if (typeof jit !== "undefined") jit.prepare(collect);
    for (let i = 0; i < 10; ++i) expect(collect([1, 2, 3])).toEqual([1, 2, 3]);
    if (typeof jit !== "undefined") expect(jit.compile(collect)).toBe(jit.enabled);
}

function* reversed() {
    for (let i = this.length - 1; i >= 0; --i) yield this[i];
}

test("array iteration calls an iterator method assigned to Array.prototype", () => {
    compileIteration();
    Array.prototype[Symbol.iterator] = reversed;
    try {
        expect(collect([1, 2, 3])).toEqual([3, 2, 1]);
    } finally {
        Array.prototype[Symbol.iterator] = originalIteratorMethod;
    }
    expect(collect([1, 2, 3])).toEqual([1, 2, 3]);
});

test("array iteration calls an own iterator method of the array", () => {
    compileIteration();
    const array = [1, 2, 3];
    array[Symbol.iterator] = reversed;
    expect(collect(array)).toEqual([3, 2, 1]);
    const arrayWithNamedProperty = [1, 2, 3];
    arrayWithNamedProperty.name = "array";
    expect(collect(arrayWithNamedProperty)).toEqual([1, 2, 3]);
});

test("array iteration calls an iterator method assigned after unrelated properties changed", () => {
    compileIteration();
    Array.prototype.unrelated = 1;
    expect(collect([1, 2, 3])).toEqual([1, 2, 3]);
    delete Array.prototype.unrelated;
    compileIteration();
    Object.defineProperty(Array.prototype, Symbol.iterator, {
        value: reversed,
        configurable: true,
        writable: true,
    });
    try {
        expect(collect([1, 2, 3])).toEqual([3, 2, 1]);
    } finally {
        Array.prototype[Symbol.iterator] = originalIteratorMethod;
    }
});
