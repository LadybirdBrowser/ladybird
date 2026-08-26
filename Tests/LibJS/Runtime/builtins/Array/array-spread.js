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
