test("length is 2", () => {
    expect(Object.defineProperties).toHaveLength(2);
});

describe("errors", () => {
    test("non-object argument", () => {
        expect(() => Object.defineProperties(42, {})).toThrowWithMessage(
            TypeError,
            "Object argument is not an object"
        );
    });
});

describe("normal behavior", () => {
    test("returns given object", () => {
        const o = {};
        expect(Object.defineProperties(o, {})).toBe(o);
    });

    test("defines given properties on object", () => {
        const properties = {
            foo: {
                writable: true,
                configurable: true,
                value: "foo",
            },
            bar: {
                enumerable: true,
                value: "bar",
            },
            baz: {
                get() {},
                set() {},
            },
        };
        const o = Object.defineProperties({}, properties);
        expect(Object.getOwnPropertyNames(o)).toEqual(["foo", "bar", "baz"]);
        expect(Object.getOwnPropertyDescriptor(o, "foo")).toEqual({
            value: "foo",
            writable: true,
            enumerable: false,
            configurable: true,
        });
        expect(Object.getOwnPropertyDescriptor(o, "bar")).toEqual({
            value: "bar",
            writable: false,
            enumerable: true,
            configurable: false,
        });
        expect(Object.getOwnPropertyDescriptor(o, "baz")).toEqual({
            get: properties.baz.get,
            set: properties.baz.set,
            enumerable: false,
            configurable: false,
        });
    });
});
