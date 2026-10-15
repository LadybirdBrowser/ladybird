const ArrayIteratorPrototype = Object.getPrototypeOf([][Symbol.iterator]());
const originalNext = ArrayIteratorPrototype.next;

function spread(array) {
    return [...array];
}

function destructure(array) {
    const [first, second] = array;
    return [first, second];
}

function collect(array) {
    const values = [];
    for (const value of array) values.push(value);
    return values;
}

function compileIteration() {
    for (const f of [spread, destructure, collect]) {
        if (typeof jit !== "undefined") jit.prepare(f);
        for (let i = 0; i < 10; ++i) f([1, 2, 3]);
        if (typeof jit !== "undefined") expect(jit.compile(f)).toBe(jit.enabled);
    }
}

let nextCalls = 0;
function countingNext() {
    ++nextCalls;
    return originalNext.call(this);
}

function expectCountingNext() {
    nextCalls = 0;
    expect(spread([1, 2, 3])).toEqual([1, 2, 3]);
    expect(nextCalls).toBe(4);
    expect(destructure([1, 2, 3])).toEqual([1, 2]);
    expect(nextCalls).toBe(6);
    expect(collect([1, 2, 3])).toEqual([1, 2, 3]);
    expect(nextCalls).toBe(10);
}

test("array iteration gets next from an accessor on %ArrayIteratorPrototype%", () => {
    compileIteration();
    let getterCalls = 0;
    Object.defineProperty(ArrayIteratorPrototype, "next", {
        get() {
            ++getterCalls;
            return countingNext;
        },
        configurable: true,
    });
    expectCountingNext();
    expect(getterCalls).toBe(3);
});
