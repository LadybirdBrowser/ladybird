test("basic functionality", () => {
    function Foo() {}
    Object.defineProperty(Foo, Symbol.hasInstance, {
        value: instance => instance === 2,
    });

    expect(new Foo() instanceof Foo).toBeFalse();
    expect(2 instanceof Foo).toBeTrue();
});
