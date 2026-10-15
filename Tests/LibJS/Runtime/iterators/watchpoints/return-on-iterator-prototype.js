const ArrayIteratorPrototype = Object.getPrototypeOf([][Symbol.iterator]());
const IteratorPrototype = Object.getPrototypeOf(ArrayIteratorPrototype);

function destructure(array) {
    const [first] = array;
    return first;
}

function breakAfterFirst(array) {
    for (const value of array) return value;
}

function compileIteration() {
    for (const f of [destructure, breakAfterFirst]) {
        if (typeof jit !== "undefined") jit.prepare(f);
        for (let i = 0; i < 10; ++i) f([1, 2, 3]);
        if (typeof jit !== "undefined") expect(jit.compile(f)).toBe(jit.enabled);
    }
}

let returnCalls = 0;
function recordingReturn() {
    ++returnCalls;
    expect(Object.getPrototypeOf(this)).toBe(ArrayIteratorPrototype);
    expect(this.next()).toEqual({ value: 2, done: false });
    return {};
}

function expectRecordingReturn() {
    returnCalls = 0;
    expect(destructure([1, 2, 3])).toBe(1);
    expect(breakAfterFirst([1, 2, 3])).toBe(1);
    expect(returnCalls).toBe(2);
}

test("closing an array iterator calls a return method on %IteratorPrototype%", () => {
    compileIteration();
    IteratorPrototype.return = recordingReturn;
    expectRecordingReturn();
});
