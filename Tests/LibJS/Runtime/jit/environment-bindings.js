// Bindings at static environment coordinates are read and written in place, and their slow paths run where a binding
// is uninitialized or immutable.

test("reading an uninitialized binding throws", () => {
    function readsEarly(early) {
        const read = () => value;
        if (early) return read();
        let value = 1;
        return read();
    }
    for (let i = 0; i < 200; ++i) expect(readsEarly(false)).toBe(1);
    expect(() => readsEarly(true)).toThrowWithMessage(ReferenceError, "value");
});

test("assigning a constant binding throws", () => {
    function assigns(assign) {
        const constant = 1;
        const write = () => {
            if (assign) constant = 2;
            return constant;
        };
        return write();
    }
    for (let i = 0; i < 200; ++i) expect(assigns(false)).toBe(1);
    expect(() => assigns(true)).toThrow(TypeError);
});

test("assigning the name of a function expression does nothing in sloppy code", () => {
    const named = function name(assign) {
        const write = () => {
            if (assign) name = 1;
            return typeof name;
        };
        return write();
    };
    for (let i = 0; i < 200; ++i) expect(named(i % 2 === 0)).toBe("function");
});

test("closures in loops see their own bindings", () => {
    function closures() {
        const functions = [];
        for (let i = 0; i < 5; ++i) functions.push(() => i);
        let sum = 0;
        for (const f of functions) sum = sum * 10 + f();
        return sum;
    }
    for (let i = 0; i < 200; ++i) expect(closures()).toBe(1234);
});

test("bindings written in loops are read back", () => {
    function counts() {
        let count = 0;
        const increment = () => ++count;
        for (let i = 0; i < 10; ++i) increment();
        return count;
    }
    for (let i = 0; i < 200; ++i) expect(counts()).toBe(10);
});
