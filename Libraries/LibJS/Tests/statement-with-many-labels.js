test("basic support for statement with many labels", () => {
    function foo() {
        a: b: c: for (;;) {
            break b;
        }
        return 1;
    }
    expect(foo()).toBe(1);
});
