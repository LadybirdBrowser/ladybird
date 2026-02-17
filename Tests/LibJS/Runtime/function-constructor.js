test("dynamic function can access global variables", () => {
    globalThis.__calls = 0;
    var f = new Function("__calls += 1; return __calls;");
    expect(f()).toBe(1);
    expect(f()).toBe(2);
    expect(globalThis.__calls).toBe(2);
    delete globalThis.__calls;
});

test("dynamic function uses GetBinding not GetGlobal", () => {
    // Dynamic functions must use GetBinding (scope chain traversal) rather
    // than GetGlobal (realm's global object) because a dynamic function's
    // realm() can differ from its environment's realm when created via
    // cross-realm Reflect.construct. This test verifies the basic case
    // works: globals are accessed correctly via the scope chain.
    globalThis.__x = 42;
    var f = new Function("return __x;");
    expect(f()).toBe(42);
    delete globalThis.__x;
});

test("dynamic function with arguments object", () => {
    var f = new Function("a", "b", "return arguments.length;");
    expect(f(1, 2, 3)).toBe(3);
});

test("dynamic generator function can access global variables", () => {
    globalThis.__genCalls = 0;
    var GeneratorFunction = function* () {}.constructor;
    var g = new GeneratorFunction("__genCalls += 1; yield __genCalls; __genCalls += 1; yield __genCalls;");
    var iter = g();
    expect(iter.next().value).toBe(1);
    expect(iter.next().value).toBe(2);
    expect(globalThis.__genCalls).toBe(2);
    delete globalThis.__genCalls;
});
