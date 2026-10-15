const ArrayIteratorPrototype = Object.getPrototypeOf([][Symbol.iterator]());
const IteratorPrototype = Object.getPrototypeOf(ArrayIteratorPrototype);
const originalNext = ArrayIteratorPrototype.next;

function spread(array) {
    return [...array];
}

function destructure(array) {
    const [first] = array;
    return first;
}

function breakAfterFirst(array) {
    for (const value of array) return value;
}

function compileIteration() {
    for (const f of [spread, destructure, breakAfterFirst]) {
        if (typeof jit !== "undefined") jit.prepare(f);
        for (let i = 0; i < 10; ++i) f([1, 2, 3]);
        if (typeof jit !== "undefined") expect(jit.compile(f)).toBe(jit.enabled);
    }
}

function changeUnrelatedProperties() {
    for (const object of [ArrayIteratorPrototype, IteratorPrototype, Object.prototype]) {
        Object.defineProperty(object, "unrelated", { value: 1, configurable: true, writable: true });
        expect(spread([1, 2, 3])).toEqual([1, 2, 3]);
        expect(destructure([1, 2, 3])).toBe(1);
        expect(breakAfterFirst([1, 2, 3])).toBe(1);
        delete object.unrelated;
        expect(spread([1, 2, 3])).toEqual([1, 2, 3]);
    }
}

test("array iteration calls a return method defined after unrelated properties changed", () => {
    compileIteration();
    changeUnrelatedProperties();
    compileIteration();

    let returnCalls = 0;
    Object.defineProperty(Object.prototype, "return", {
        value() {
            ++returnCalls;
            return {};
        },
        configurable: true,
        writable: true,
    });
    try {
        expect(destructure([1, 2, 3])).toBe(1);
        expect(breakAfterFirst([1, 2, 3])).toBe(1);
        expect(returnCalls).toBe(2);
    } finally {
        delete Object.prototype.return;
    }
    expect(breakAfterFirst([1, 2, 3])).toBe(1);
    expect(returnCalls).toBe(2);
});

test("array iteration calls a next method defined after unrelated properties changed", () => {
    compileIteration();
    changeUnrelatedProperties();
    compileIteration();

    let nextCalls = 0;
    Object.defineProperty(ArrayIteratorPrototype, "next", {
        value() {
            ++nextCalls;
            return originalNext.call(this);
        },
        configurable: true,
        writable: true,
    });
    try {
        expect(spread([1, 2, 3])).toEqual([1, 2, 3]);
        expect(nextCalls).toBe(4);
    } finally {
        Object.defineProperty(ArrayIteratorPrototype, "next", { value: originalNext });
    }
    expect(spread([1, 2, 3])).toEqual([1, 2, 3]);
    expect(nextCalls).toBe(4);
});
