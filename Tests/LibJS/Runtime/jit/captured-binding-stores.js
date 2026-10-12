function compiled(f, ...warmupArguments) {
    jit.prepare(f);
    for (let i = 0; i < 10; ++i) f(...warmupArguments);
    expect(jit.compile(f)).toBe(jit.enabled);
    return f;
}

test("compiled stores to captured var bindings", () => {
    function outer() {
        var top = 0;
        function store(value) {
            top = value;
            return top;
        }
        return [store, () => top];
    }
    const [store, read] = outer();
    compiled(store, 1);
    expect(store(42)).toBe(42);
    expect(read()).toBe(42);
});

test("compiled stores to captured const bindings still throw", () => {
    function outer() {
        const fixed = 1;
        let calls = 0;
        function store(value) {
            calls = (calls + 1) | 0;
            if (value === "assign") fixed = 2;
            return calls;
        }
        return [store, () => fixed];
    }
    const [store, read] = outer();
    compiled(store, 0);
    expect(() => store("assign")).toThrowWithMessage(TypeError, "Invalid assignment to const variable");
    expect(read()).toBe(1);
});

test("compiled stores to captured let bindings before their initialization still throw", () => {
    function outer(storeFirst) {
        function store(value) {
            late = value;
            return late;
        }
        if (storeFirst) store(1);
        let late = 0;
        return store;
    }
    const store = outer(false);
    compiled(store, 1);
    expect(store(5)).toBe(5);
    expect(() => outer(true)).toThrowWithMessage(ReferenceError, "Binding late is not initialized");
});
