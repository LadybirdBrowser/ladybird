const originalIterator = Array.prototype[Symbol.iterator];
const arrayIteratorPrototype = Object.getPrototypeOf([][Symbol.iterator]());
const originalNext = arrayIteratorPrototype.next;

test("builtins iterate arrays like their iterators do", () => {
    expect([...new Set([1, 2, 2, 3])]).toEqual([1, 2, 3]);
    expect([
        ...new Map([
            [1, "a"],
            [2, "b"],
        ]),
    ]).toEqual([
        [1, "a"],
        [2, "b"],
    ]);
    expect(Object.fromEntries([["a", 1]])).toEqual({ a: 1 });
    expect(Array.from([1, 2, 3])).toEqual([1, 2, 3]);
    expect(Array.from([1, , 3])).toEqual([1, undefined, 3]);
    const source = [1, 2];
    const copy = Array.from(source);
    copy.push(3);
    expect(source).toEqual([1, 2]);
});

test("a replaced @@iterator of Array.prototype is used", () => {
    Array.prototype[Symbol.iterator] = function* () {
        yield "replaced";
    };
    try {
        expect([...new Set([1, 2])]).toEqual(["replaced"]);
        expect(Array.from([1, 2])).toEqual(["replaced"]);
    } finally {
        Array.prototype[Symbol.iterator] = originalIterator;
    }
});

test("an own @@iterator of an array is used", () => {
    const array = [1, 2];
    array[Symbol.iterator] = function* () {
        yield "own";
    };
    expect([...new Set(array)]).toEqual(["own"]);
    expect(Array.from(array)).toEqual(["own"]);
});

test("a replaced next of array iterators is called", () => {
    let calls = 0;
    arrayIteratorPrototype.next = function () {
        ++calls;
        return originalNext.call(this);
    };
    try {
        expect([...new Set([1, 2])]).toEqual([1, 2]);
        expect(Array.from([1, 2])).toEqual([1, 2]);
        expect(calls).toBe(6);
    } finally {
        arrayIteratorPrototype.next = originalNext;
    }
});

test("holes read the prototype chain", () => {
    Array.prototype[1] = "from prototype";
    try {
        expect([...new Set([1, , 3])]).toEqual([1, "from prototype", 3]);
        expect(Array.from([1, , 3])).toEqual([1, "from prototype", 3]);
    } finally {
        delete Array.prototype[1];
    }
});

test("adders see the array grow while it is iterated", () => {
    const array = [1, 2];
    class GrowingSet extends Set {
        add(value) {
            if (value < 4) array.push(value + 2);
            return super.add(value);
        }
    }
    expect([...new GrowingSet(array)]).toEqual([1, 2, 3, 4, 5]);
});

test("an iterator return method added while iterating is called on an abrupt completion", () => {
    let returnCalls = 0;
    class ThrowingSet extends Set {
        add(value) {
            if (value === 1) {
                arrayIteratorPrototype.return = function () {
                    ++returnCalls;
                    expect(Object.getPrototypeOf(this)).toBe(arrayIteratorPrototype);
                    return {};
                };
                return this;
            }
            throw new Error("adder failed");
        }
    }
    try {
        expect(() => new ThrowingSet([1, 2])).toThrowWithMessage(Error, "adder failed");
        expect(returnCalls).toBe(1);
    } finally {
        delete arrayIteratorPrototype.return;
    }
});

test("an entry that is not an object throws", () => {
    expect(() => new Map([1])).toThrow(TypeError);
});
