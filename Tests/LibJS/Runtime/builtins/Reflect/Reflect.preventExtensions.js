describe("errors", () => {
    test("target must be an object", () => {
        [null, undefined, "foo", 123, NaN, Infinity].forEach(value => {
            expect(() => {
                Reflect.preventExtensions(value);
            }).toThrowWithMessage(TypeError, `${value} is not an object`);
        });
    });
});

describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Reflect.preventExtensions).toHaveLength(1);
    });

    test("properties cannot be added", () => {
        var o = {};
        o.foo = "foo";
        expect(Reflect.preventExtensions(o)).toBeTrue();
        o.bar = "bar";
        expect(o.foo).toBe("foo");
        expect(o.bar).toBeUndefined();
    });

    test("modifying existing properties", () => {
        const o = {};
        o.foo = "foo";
        expect(Reflect.preventExtensions(o)).toBeTrue();
        o.foo = "bar";
        expect(o.foo).toBe("bar");
    });

    test("deleting existing properties", () => {
        const o = { foo: "bar" };
        Reflect.preventExtensions(o);
        delete o.foo;
        expect(o).not.toHaveProperty("foo");
    });
});
