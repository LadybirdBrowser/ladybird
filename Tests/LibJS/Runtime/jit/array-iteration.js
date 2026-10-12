function compileIteration(f, ...args) {
    jit.prepare(f);
    for (let i = 0; i < 10; ++i) f(...args);
    expect(jit.compile(f)).toBe(jit.enabled);
    return f;
}

function collect(iterable) {
    const values = [];
    for (const value of iterable) values.push(value);
    return values;
}

function firstTwo(iterable) {
    const [first, second] = iterable;
    return [first, second];
}

test("for-of and destructuring step through packed and holey arrays", () => {
    compileIteration(collect, [1, 2, 3]);
    compileIteration(firstTwo, [1, 2, 3]);
    expect(collect([4, 5, 6])).toEqual([4, 5, 6]);
    expect(collect([])).toEqual([]);
    expect(collect([1, , 3])).toEqual([1, undefined, 3]);
    expect(firstTwo([7])).toEqual([7, undefined]);
    expect(firstTwo([7, 8, 9])).toEqual([7, 8]);
    expect(jit.exitCount(collect)).toBe(0);
});

test("holes read the prototype chain", () => {
    compileIteration(collect, [1, 2, 3]);
    Array.prototype[1] = "from prototype";
    try {
        expect(collect([1, , 3])).toEqual([1, "from prototype", 3]);
    } finally {
        delete Array.prototype[1];
    }
});

test("holey arrays iterate up to their length past their storage", () => {
    const count = compileIteration(new Function("array", "let n = 0; for (const value of array) ++n; return n;"), [
        1,
        ,
        3,
    ]);
    expect(count(new Array(5))).toBe(5);
    const grown = [1, 2, 3];
    grown.length = 40;
    expect(count(grown)).toBe(40);
    expect(collect(Object.assign(new Array(3), { 1: "set" }))).toEqual([undefined, "set", undefined]);
});

test("arrays changed while iterating", () => {
    compileIteration(collect, [1, 2, 3]);
    const growing = [1, 2];
    const seen = [];
    for (const value of growing) {
        seen.push(value);
        if (growing.length < 5) growing.push(value * 10);
    }
    expect(seen).toEqual([1, 2, 10, 20, 100]);

    const shrinking = [1, 2, 3, 4];
    const kept = [];
    const step = new Function("array", "kept", "for (const value of array) { kept.push(value); array.length = 2; }");
    compileIteration(step, [1, 2, 3], []);
    step(shrinking, kept);
    expect(kept).toEqual([1, 2]);
});

test("other iterables and iterators take the slow path", () => {
    compileIteration(collect, [1, 2, 3]);
    expect(collect(new Uint8Array([1, 2]))).toEqual([1, 2]);
    expect(collect("ab")).toEqual(["a", "b"]);
    expect(collect(new Set([3, 4]))).toEqual([3, 4]);
    expect(collect([5, 6].keys())).toEqual([0, 1]);
    expect(collect([5, 6].entries())).toEqual([
        [0, 5],
        [1, 6],
    ]);
    expect(collect(new Proxy([1, 2], {}))).toEqual([1, 2]);
    class MyArray extends Array {}
    expect(collect(MyArray.of(8, 9))).toEqual([8, 9]);
});

test("own and replaced iterator methods are called", () => {
    compileIteration(collect, [1, 2, 3]);
    const own = [1, 2];
    own[Symbol.iterator] = function* () {
        yield "own";
    };
    expect(collect(own)).toEqual(["own"]);

    const values = Array.prototype[Symbol.iterator];
    Array.prototype[Symbol.iterator] = function* () {
        yield "replaced";
    };
    try {
        expect(collect([1, 2])).toEqual(["replaced"]);
    } finally {
        Array.prototype[Symbol.iterator] = values;
    }
    expect(collect([1, 2])).toEqual([1, 2]);

    let getterCalls = 0;
    Object.defineProperty(Array.prototype, Symbol.iterator, {
        get() {
            ++getterCalls;
            return values;
        },
        configurable: true,
    });
    try {
        expect(collect([1, 2])).toEqual([1, 2]);
        expect(getterCalls).toBe(1);
    } finally {
        Object.defineProperty(Array.prototype, Symbol.iterator, {
            value: values,
            writable: true,
            configurable: true,
        });
    }
});

test("a replaced next method is called", () => {
    compileIteration(collect, [1, 2, 3]);
    const iteratorPrototype = Object.getPrototypeOf([][Symbol.iterator]());
    const next = iteratorPrototype.next;
    let calls = 0;
    iteratorPrototype.next = function () {
        ++calls;
        return next.call(this);
    };
    try {
        expect(collect([1, 2])).toEqual([1, 2]);
        expect(calls).toBe(3);
    } finally {
        iteratorPrototype.next = next;
    }
    expect(collect([1, 2])).toEqual([1, 2]);
});

test("array-likes iterate up to their length property", () => {
    compileIteration(collect, [1, 2, 3]);
    const shorter = { 0: "a", 1: "b", length: 1, [Symbol.iterator]: Array.prototype.values };
    expect(collect(shorter)).toEqual(["a"]);
    const longer = { 0: "a", length: 3, [Symbol.iterator]: Array.prototype.values };
    expect(collect(longer)).toEqual(["a", undefined, undefined]);
    const argumentsWithLength = (function () {
        "use strict";
        arguments.length = 1;
        return arguments;
    })(1, 2, 3);
    expect(collect(argumentsWithLength)).toEqual([1]);
});
