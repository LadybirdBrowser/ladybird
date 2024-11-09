test("issue #6766, nested functions should not leak to global object", () => {
    function foo() {
        function bar() {
            function baz() {
                return 42;
            }
            return baz();
        }
        return bar();
    }
    expect(foo()).toBe(42);
    expect(globalThis.foo).toBeUndefined();
    expect(globalThis.bar).toBeUndefined();
    expect(globalThis.baz).toBeUndefined();
});
