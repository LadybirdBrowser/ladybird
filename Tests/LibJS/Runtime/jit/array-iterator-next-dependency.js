// Compiled code starts for-of loops over arrays without asking the runtime while the realm's array iterators have
// their builtin next method. Replacing it, also while the code runs, makes the next loop start call the new method
// (see array-iteration-dependencies.js).

function compiled(parameters, body, ...warmUpArguments) {
    const f = new Function(...parameters, body);
    jit.prepare(f);
    for (let i = 0; i < 10; ++i) f(...warmUpArguments);
    expect(jit.compile(f)).toBe(jit.enabled);
    return f;
}

// NB: The code calls the same function between its loops every time, so that it goes on in compiled code after it.
let action = () => {};
const between = () => action();

test("a next method of array iterators replaced while compiled code runs is called from the next loop on", () => {
    const iteratorPrototype = Object.getPrototypeOf([][Symbol.iterator]());
    const originalNext = iteratorPrototype.next;
    let calls = 0;
    const twoLoops = compiled(
        ["array", "between"],
        "let first = 0; for (const value of array) { first += value; between(); } let second = 0; for (const value of array) second += value; return [first, second];",
        [1, 2],
        between
    );
    action = () => {
        iteratorPrototype.next = function () {
            ++calls;
            return originalNext.call(this);
        };
    };
    try {
        expect(twoLoops([1, 2], between)).toEqual([3, 3]);
        expect(calls).toBe(3);
        action = () => {};
        expect(twoLoops([1, 2], between)).toEqual([3, 3]);
        expect(calls).toBe(9);
    } finally {
        action = () => {};
        iteratorPrototype.next = originalNext;
    }
    expect(twoLoops([1, 2], between)).toEqual([3, 3]);
    expect(calls).toBe(9);
});
