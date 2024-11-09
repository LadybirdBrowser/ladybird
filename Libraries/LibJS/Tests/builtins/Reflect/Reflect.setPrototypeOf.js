test("length is 2", () => {
    expect(Reflect.setPrototypeOf).toHaveLength(2);
});

describe("errors", () => {
    test("target must be an object", () => {
        [null, undefined, "foo", 123, NaN, Infinity].forEach(value => {
            expect(() => {
                Reflect.setPrototypeOf(value);
            }).toThrowWithMessage(TypeError, `${value} is not an object`);
        });
    });

    test("prototype must be an object or null", () => {
        [undefined, "foo", 123, NaN, Infinity].forEach(value => {
            expect(() => {
                Reflect.setPrototypeOf({}, value);
            }).toThrowWithMessage(TypeError, "Prototype must be an object or null");
        });
    });
});

describe("normal behavior", () => {
    test("setting prototype of regular object", () => {
        expect(Reflect.setPrototypeOf({}, null)).toBeTrue();
        expect(Reflect.setPrototypeOf({}, {})).toBeTrue();
        expect(Reflect.setPrototypeOf({}, Object.prototype)).toBeTrue();
        expect(Reflect.setPrototypeOf({}, Array.prototype)).toBeTrue();
        expect(Reflect.setPrototypeOf({}, String.prototype)).toBeTrue();
        expect(Reflect.setPrototypeOf({}, Reflect.getPrototypeOf({}))).toBeTrue();
    });

    test("setting user-defined prototype of regular object", () => {
        var o = {};
        var p = { foo: "bar" };
        expect(o.foo).toBeUndefined();
        expect(Reflect.setPrototypeOf(o, p)).toBeTrue();
        expect(o.foo).toBe("bar");
    });

    test("setting prototype of non-extensible object", () => {
        var o = {};
        Reflect.preventExtensions(o);
        expect(Reflect.setPrototypeOf(o, {})).toBeFalse();
    });

    test("setting same prototype of non-extensible object", () => {
        var o = {};
        var p = { foo: "bar" };
        expect(Reflect.setPrototypeOf(o, p)).toBeTrue();
        Reflect.preventExtensions(o);
        expect(Reflect.setPrototypeOf(o, p)).toBeTrue();
    });
});
