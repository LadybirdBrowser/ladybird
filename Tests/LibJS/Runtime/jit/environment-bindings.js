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

test("reading the truthiness of an uninitialized binding in a loop throws", () => {
    function makeReader(early) {
        const read = () => {
            let result = 0;
            for (let i = 0; i < 2; ++i) result = !value;
            return result;
        };
        if (early) return read;
        let value = undefined;
        return read;
    }
    const initialized = makeReader(false);
    const uninitialized = makeReader(true);
    jit.prepare(initialized);
    for (let i = 0; i < 10; ++i) expect(initialized()).toBeTrue();
    for (let i = 0; i < 3; ++i) {
        jit.discard(initialized);
        const compiled = jit.compile(initialized);
        if (!jit.forcesExits) expect(compiled).toBe(jit.enabled);
        expect(uninitialized).toThrowWithMessage(ReferenceError, "value");
        expect(initialized()).toBeTrue();
    }
});

test("reading the truthiness of an uninitialized local in a loop throws", () => {
    function read(clause) {
        let result = 0;
        switch (clause) {
            case 0:
                let value = undefined;
            case 1:
                for (let i = 0; i < 2; ++i) result = !value;
        }
        return result;
    }
    jit.prepare(read);
    for (let i = 0; i < 10; ++i) {
        expect(read(0)).toBeTrue();
        expect(read(2)).toBe(0);
    }
    for (let i = 0; i < 3; ++i) {
        jit.discard(read);
        const compiled = jit.compile(read);
        if (!jit.forcesExits) expect(compiled).toBe(jit.enabled);
        expect(() => read(1)).toThrowWithMessage(ReferenceError, "not initialized");
        expect(read(0)).toBeTrue();
    }
});

test("reading the truthiness of an uninitialized local in a branch of a loop throws", () => {
    function read(clause, choose) {
        let result = 0;
        switch (clause) {
            case 0:
                let value = undefined;
            case 1:
                for (let i = 0; i < 2; ++i) {
                    if (choose) result = !value;
                }
        }
        return result;
    }
    jit.prepare(read);
    for (let i = 0; i < 10; ++i) {
        expect(read(0, true)).toBeTrue();
        expect(read(0, false)).toBe(0);
    }
    for (let i = 0; i < 3; ++i) {
        jit.discard(read);
        const compiled = jit.compile(read);
        if (!jit.forcesExits) expect(compiled).toBe(jit.enabled);
        expect(() => read(1, true)).toThrowWithMessage(ReferenceError, "not initialized");
        expect(read(0, true)).toBeTrue();
    }
});

test("reading the truthiness of a local checked in both branches of a loop throws", () => {
    function read(clause, choose) {
        let result = 0;
        let copy = 0;
        switch (clause) {
            case 0:
                let value = undefined;
            case 1:
                for (let i = 0; i < 2; ++i) {
                    if (choose) copy = value;
                    else copy = value;
                    result = !copy;
                }
        }
        return result;
    }
    jit.prepare(read);
    for (let i = 0; i < 10; ++i) {
        expect(read(0, true)).toBeTrue();
        expect(read(0, false)).toBeTrue();
    }
    for (let i = 0; i < 3; ++i) {
        jit.discard(read);
        const compiled = jit.compile(read);
        if (!jit.forcesExits) expect(compiled).toBe(jit.enabled);
        expect(() => read(1, true)).toThrowWithMessage(ReferenceError, "not initialized");
        expect(read(0, true)).toBeTrue();
    }
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
