describe("errors", () => {
    test("cannot spread number in array", () => {
        expect(() => {
            [...1];
        }).toThrowWithMessage(TypeError, "1 is not iterable");
    });

    test("cannot spread object in array", () => {
        expect(() => {
            [...{}];
        }).toThrowWithMessage(TypeError, "[object Object] is not iterable");
    });
});

test("basic functionality", () => {
    expect([1, ...[2, 3], 4]).toEqual([1, 2, 3, 4]);

    let a = [2, 3];
    expect([1, ...a, 4]).toEqual([1, 2, 3, 4]);

    let obj = { a: [2, 3] };
    expect([1, ...obj.a, 4]).toEqual([1, 2, 3, 4]);

    expect([...[], ...[...[1, 2, 3]], 4]).toEqual([1, 2, 3, 4]);
});

test("observes custom array iterator accessors", () => {
    const array = [1, 2];
    const originalIterator = Array.prototype[Symbol.iterator];
    let getterCalls = 0;
    Object.defineProperty(array, Symbol.iterator, {
        configurable: true,
        get() {
            ++getterCalls;
            this.push(3);
            return originalIterator;
        },
    });

    expect([...array]).toEqual([1, 2, 3]);
    expect(getterCalls).toBe(1);
});

test("exhausts iterators when ArrayIteratorPrototype.next is an accessor", () => {
    const iteratorPrototype = Object.getPrototypeOf([][Symbol.iterator]());
    const originalDescriptor = Object.getOwnPropertyDescriptor(iteratorPrototype, "next");
    let iterator;

    Object.defineProperty(iteratorPrototype, "next", {
        configurable: true,
        get() {
            iterator = this;
            return originalDescriptor.value;
        },
    });

    try {
        expect([...[1, 2, 3]]).toEqual([1, 2, 3]);
        expect(iterator.next()).toEqual({ value: undefined, done: true });
    } finally {
        Object.defineProperty(iteratorPrototype, "next", originalDescriptor);
    }
});

test("observes replacement ArrayIteratorPrototype.next methods", () => {
    const iteratorPrototype = Object.getPrototypeOf([][Symbol.iterator]());
    const originalNext = iteratorPrototype.next;
    let calls = 0;

    iteratorPrototype.next = function () {
        ++calls;
        return originalNext.call(this);
    };

    try {
        expect([...[1, 2, 3]]).toEqual([1, 2, 3]);
        expect(calls).toBe(4);
    } finally {
        iteratorPrototype.next = originalNext;
    }
});

test("elisions after spread remain holes", () => {
    let array = [...[], ,];
    expect(array).toHaveLength(1);
    expect(array.hasOwnProperty(0)).toBeFalse();
    expect(0 in array).toBeFalse();
    expect(array[0]).toBeUndefined();
    expect(String(array[0])).toBe("undefined");

    array = [1, ...[], ,];
    expect(array).toHaveLength(2);
    expect(array.hasOwnProperty(0)).toBeTrue();
    expect(array.hasOwnProperty(1)).toBeFalse();
    expect(1 in array).toBeFalse();
    expect(array[1]).toBeUndefined();
});

test("allows assignment expressions", () => {
    expect("([ ...a = { hello: 'world' } ])").toEval();
    expect("([ ...a += 'hello' ])").toEval();
    expect("([ ...a -= 'hello' ])").toEval();
    expect("([ ...a **= 'hello' ])").toEval();
    expect("([ ...a *= 'hello' ])").toEval();
    expect("([ ...a /= 'hello' ])").toEval();
    expect("([ ...a %= 'hello' ])").toEval();
    expect("([ ...a <<= 'hello' ])").toEval();
    expect("([ ...a >>= 'hello' ])").toEval();
    expect("([ ...a >>>= 'hello' ])").toEval();
    expect("([ ...a &= 'hello' ])").toEval();
    expect("([ ...a ^= 'hello' ])").toEval();
    expect("([ ...a |= 'hello' ])").toEval();
    expect("([ ...a &&= 'hello' ])").toEval();
    expect("([ ...a ||= 'hello' ])").toEval();
    expect("([ ...a ??= 'hello' ])").toEval();
    expect("function* test() { return ([ ...yield a ]); }").toEval();
});

describe("spreading Sets and Maps", () => {
    test("basic functionality", () => {
        const set = new Set([1, "two", 3]);
        expect([0, ...set, 4]).toEqual([0, 1, "two", 3, 4]);
        expect([...new Set()]).toEqual([]);

        const map = new Map([
            ["a", 1],
            ["b", 2],
        ]);
        const entries = [...map];
        expect(entries).toEqual([
            ["a", 1],
            ["b", 2],
        ]);
        expect(entries[0]).not.toBe(entries[1]);
        expect([...map.keys()]).toEqual(["a", "b"]);
        expect([...map.values()]).toEqual([1, 2]);
        expect([...set.entries()]).toEqual([
            [1, 1],
            ["two", "two"],
            [3, 3],
        ]);
    });

    test("observes a patched Set.prototype[Symbol.iterator]", () => {
        const original = Set.prototype[Symbol.iterator];
        Set.prototype[Symbol.iterator] = function* () {
            yield "patched";
        };
        try {
            expect([...new Set([1, 2])]).toEqual(["patched"]);
        } finally {
            Set.prototype[Symbol.iterator] = original;
        }
    });

    test("observes a patched %SetIteratorPrototype%.next", () => {
        const setIteratorPrototype = Object.getPrototypeOf(new Set().values());
        const originalNext = setIteratorPrototype.next;
        let calls = 0;
        setIteratorPrototype.next = function () {
            ++calls;
            return originalNext.call(this);
        };
        try {
            expect([...new Set([1, 2])]).toEqual([1, 2]);
            expect(calls).toBe(3);
        } finally {
            setIteratorPrototype.next = originalNext;
        }
    });

    test("observes a patched Map.prototype[Symbol.iterator]", () => {
        const original = Map.prototype[Symbol.iterator];
        Map.prototype[Symbol.iterator] = Map.prototype.values;
        try {
            expect([...new Map([["k", "v"]])]).toEqual(["v"]);
        } finally {
            Map.prototype[Symbol.iterator] = original;
        }
    });

    test("observes a %MapIteratorPrototype%.next getter", () => {
        const mapIteratorPrototype = Object.getPrototypeOf(new Map().entries());
        const originalDescriptor = Object.getOwnPropertyDescriptor(mapIteratorPrototype, "next");
        let getterCalls = 0;
        Object.defineProperty(mapIteratorPrototype, "next", {
            configurable: true,
            get() {
                ++getterCalls;
                return originalDescriptor.value;
            },
        });
        try {
            expect([...new Map([["k", "v"]])]).toEqual([["k", "v"]]);
            expect(getterCalls).toBe(1);
        } finally {
            Object.defineProperty(mapIteratorPrototype, "next", originalDescriptor);
        }
    });

    test("observes a Symbol.iterator getter on a Set", () => {
        const set = new Set([1]);
        Object.defineProperty(set, Symbol.iterator, {
            get() {
                set.add(2);
                return Set.prototype.values;
            },
        });
        expect([...set]).toEqual([1, 2]);
    });
});
