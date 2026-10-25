function compiledSplice(itemCount) {
    const parameters = ["array", "start", "count", "x", "y", "z"].slice(0, itemCount + 3);
    const call = parameters.slice(1).join(", ");
    const splice = new Function(...parameters, `return array.splice(${call});`);
    jit.prepare(splice);
    for (let i = 0; i < 10; ++i) splice([1, 2, 3, 4], 1, 1, 5, 6, 7);
    expect(jit.compile(splice)).toBe(jit.enabled);
    return splice;
}

test("packed splices move both overlapping directions and return removed elements", () => {
    for (let itemCount = 0; itemCount <= 3; ++itemCount) {
        const splice = compiledSplice(itemCount);
        const reference = new Function("a", "start", "count", "items", "return a.splice(start, count, ...items);");
        jit.neverCompile(reference);
        const items = [{ name: "one" }, "two", 3.5].slice(0, itemCount);
        for (const length of [1, 2, 4, 7, 64, 1025]) {
            const values = Array.from({ length }, (_, i) => i);
            for (const start of [-0x80000000, -length - 1, -1, 0, 1, length - 1, length, 0x7fffffff]) {
                for (const count of [-0x80000000, -1, 0, 1, 2, length, 0x7fffffff]) {
                    const actual = values.slice();
                    const expected = values.slice();
                    expect(splice(actual, start, count, ...items)).toEqual(reference(expected, start, count, items));
                    expect(actual).toEqual(expected);
                }
            }
        }
    }
});

test("empty and holey arrays keep their ordinary splice behavior", () => {
    const splice = compiledSplice(2);
    const empty = [];
    expect(splice(empty, 0, 10, "a", "b")).toEqual([]);
    expect(empty).toEqual(["a", "b"]);
    const holey = [1, , 3, , 5];
    const removed = splice(holey, 1, 3, "a", "b");
    expect(removed.length).toBe(3);
    expect(0 in removed).toBeFalse();
    expect(removed[1]).toBe(3);
    expect(2 in removed).toBeFalse();
    expect(holey).toEqual([1, "a", "b", 5]);
});

test("argument coercions and changed methods run once", () => {
    const splice = compiledSplice(1);
    let calls = 0;
    const array = [1, 2, 3];
    const start = {
        valueOf() {
            ++calls;
            return 1;
        },
    };
    expect(splice(array, start, 1, "x")).toEqual([2]);
    expect(calls).toBe(1);
    expect(array).toEqual([1, "x", 3]);
    array.splice = function (...args) {
        ++calls;
        return args;
    };
    expect(splice(array, 0, 2, "y")).toEqual([0, 2, "y"]);
    expect(calls).toBe(2);
});

test("custom species is observed before changing the source array", () => {
    const splice = compiledSplice(3);
    const array = [1, 2, 3];
    let calls = 0;
    array.constructor = {
        [Symbol.species]: function (length) {
            ++calls;
            expect(length).toBe(1);
            expect(array).toEqual([1, 2, 3]);
            return { created: true };
        },
    };
    expect(splice(array, 1, 1, "a", "b", "c")).toEqual({ 0: 2, length: 1, created: true });
    expect(calls).toBe(1);
    expect([...array]).toEqual([1, "a", "b", "c", 3]);
});

test("non-writable arrays throw through the ordinary call", () => {
    const splice = compiledSplice(1);
    expect(() => splice(Object.freeze([1, 2, 3]), 1, 1, 4)).toThrow(TypeError);
    const array = [1, 2, 3];
    Object.defineProperty(array, "length", { writable: false });
    expect(() => splice(array, 1, 1, 4)).toThrow(TypeError);
    expect(array).toEqual([1, 4, 3]);
});

test("objects inserted through stack arguments survive allocation and collection", () => {
    const splice = new Function("array", "i", "return array.splice(0, array.length, { i }, [i + 1], { next: i + 2 });");
    jit.prepare(splice);
    for (let i = 0; i < 10; ++i) splice([null], i);
    expect(jit.compile(splice)).toBe(jit.enabled);
    const array = [null];
    for (let i = 0; i < 5000; ++i) {
        splice(array, i);
        expect(array).toEqual([{ i }, [i + 1], { next: i + 2 }]);
    }
});

test("values live across a splice keep their registers and spill slots", () => {
    const compute = new Function(
        "array",
        "start",
        "count",
        "x",
        "y",
        "z",
        `const before = start * 11 + count * 7 + x * 5 + y * 3 + z;
        const removed = array.splice(start, count, x, y, z);
        return [before, start, count, x, y, z, removed, array];`
    );
    jit.prepare(compute);
    for (let i = 0; i < 10; ++i) compute([1, 2, 3, 4], 1, 2, 5, 6, 7);
    expect(jit.compile(compute)).toBe(jit.enabled);
    expect(compute([1, 2, 3, 4], 1, 2, 5, 6, 7)).toEqual([75, 1, 2, 5, 6, 7, [2, 3], [1, 5, 6, 7, 4]]);
});

test("inherited setters see inserted indices", () => {
    const splice = compiledSplice(1);
    const seen = [];
    const prototype = Object.create(Array.prototype);
    Object.defineProperty(prototype, "3", {
        set(value) {
            seen.push(value);
        },
    });
    const array = [1, 2, 3];
    Object.setPrototypeOf(array, prototype);
    expect(splice(array, 3, 0, "x")).toEqual([]);
    expect(seen).toEqual(["x"]);
    expect(Object.hasOwn(array, "3")).toBeFalse();
    expect(array.length).toBe(4);
});

test("changes to the default species are observed by compiled calls", () => {
    const splice = compiledSplice(1);
    class Removed extends Array {}
    const descriptor = Object.getOwnPropertyDescriptor(Array, Symbol.species);
    let calls = 0;
    Object.defineProperty(Array, Symbol.species, {
        configurable: true,
        get() {
            ++calls;
            return Removed;
        },
    });
    try {
        const array = [1, 2, 3];
        const removed = splice(array, 1, 1, "x");
        expect(removed).toBeInstanceOf(Removed);
        expect(removed).toEqual([2]);
        expect(calls).toBe(1);
        expect(array).toEqual([1, "x", 3]);
    } finally {
        Object.defineProperty(Array, Symbol.species, descriptor);
    }
});
