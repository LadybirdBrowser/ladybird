test("basic functionality", () => {
    function Foo() {
        this.x = 123;
    }

    const foo = new Foo();
    expect(foo instanceof Foo).toBeTrue();
});

test("derived ES5 classes", () => {
    function Base() {
        this.is_base = true;
    }

    function Derived() {
        this.is_derived = true;
    }

    Object.setPrototypeOf(Derived.prototype, Base.prototype);

    const d = new Derived();
    expect(d instanceof Derived).toBeTrue();
    expect(d instanceof Base).toBeTrue();
});

test("issue #3930, instanceof on arrow function", () => {
    function f() {}
    const a = () => {};

    expect(() => {
        f instanceof a;
    }).toThrow(TypeError);
    expect(() => {
        a instanceof a;
    }).toThrow(TypeError);
});
