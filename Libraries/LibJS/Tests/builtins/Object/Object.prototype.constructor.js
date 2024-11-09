test("basic functionality", () => {
    [Array, BigInt, Boolean, Date, Error, Function, Number, Object, String].forEach(constructor => {
        expect(constructor.prototype.constructor).toBe(constructor);
        if (constructor !== BigInt)
            expect(Reflect.construct(constructor, []).constructor).toBe(constructor);
    });

    let o = {};
    expect(o.constructor).toBe(Object);

    a = [];
    expect(a.constructor).toBe(Array);

    expect(Object.prototype).toHaveConfigurableProperty("constructor");
    expect(Object.prototype).not.toHaveEnumerableProperty("constructor");
    expect(Object.prototype).toHaveWritableProperty("constructor");
    expect(Object.prototype).toHaveValueProperty("constructor", Object);
});
