test("basic functionality", () => {
    expect(Function.prototype[Symbol.hasInstance]).toHaveLength(1);

    function Foo() {}
    const foo = new Foo();

    expect(Function.prototype[Symbol.hasInstance].call(Foo, foo)).toBeTrue();
});
