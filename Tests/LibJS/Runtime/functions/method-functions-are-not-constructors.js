test("object literal concise methods are not constructors", () => {
    const object_method = { m() {} }.m;
    expect(() => {
        new object_method();
    }).toThrow(TypeError);
});

test("class methods are not constructors", () => {
    class C {
        m() {}
        static s() {}
    }

    expect(() => {
        new C.prototype.m();
    }).toThrow(TypeError);
    expect(() => {
        new C.s();
    }).toThrow(TypeError);
});

test("non-method functions remain constructible", () => {
    const plain_function = function () {};
    const property_function = { m: function () {} }.m;

    expect(new plain_function()).toBeInstanceOf(plain_function);
    expect(new property_function()).toBeInstanceOf(property_function);
});
