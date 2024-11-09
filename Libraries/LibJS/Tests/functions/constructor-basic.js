test("basic functionality", () => {
    function Foo() {
        this.x = 123;
    }

    expect(Foo.prototype.constructor).toBe(Foo);

    const foo = new Foo();
    expect(foo.constructor).toBe(Foo);
    expect(foo.x).toBe(123);
});
