function runFastPath() {
    for (let i = 0; i < 10; ++i) expect([1].concat([2], 3)).toEqual([1, 2, 3]);
}

test("@@isConcatSpreadable of Array.prototype is read after the fast path ran", () => {
    runFastPath();
    Array.prototype[Symbol.isConcatSpreadable] = false;
    try {
        const result = [1].concat([2]);
        expect(result.length).toBe(2);
        expect(result[0]).toEqual([1]);
        expect(result[1]).toEqual([2]);
    } finally {
        delete Array.prototype[Symbol.isConcatSpreadable];
    }
    runFastPath();
});

test("@@isConcatSpreadable of Object.prototype is read after the fast path ran", () => {
    runFastPath();
    Object.prototype[Symbol.isConcatSpreadable] = false;
    try {
        expect([1].concat([2]).length).toBe(2);
    } finally {
        delete Object.prototype[Symbol.isConcatSpreadable];
    }
    runFastPath();
});

test("a getter for @@isConcatSpreadable is called after the fast path ran", () => {
    runFastPath();
    let calls = 0;
    Object.defineProperty(Array.prototype, Symbol.isConcatSpreadable, {
        get() {
            ++calls;
            return true;
        },
        configurable: true,
    });
    try {
        expect([1].concat([2])).toEqual([1, 2]);
        expect(calls).toBe(2);
    } finally {
        delete Array.prototype[Symbol.isConcatSpreadable];
    }
});

test("an own @@isConcatSpreadable of an array is read", () => {
    runFastPath();
    const array = [2];
    array[Symbol.isConcatSpreadable] = false;
    expect([1].concat(array)).toEqual([1, array]);
    const receiver = [1];
    receiver[Symbol.isConcatSpreadable] = false;
    expect(receiver.concat([2])).toEqual([receiver, 2]);
});
