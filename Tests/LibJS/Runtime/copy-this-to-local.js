test("copy this to a local", () => {
    const foo = {
        foo() {
            let thisCopy = this;
            thisCopy = "oops";
            return this;
        },
    };
    expect(foo.foo()).toBe(foo);
});
