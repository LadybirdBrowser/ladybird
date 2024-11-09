test("length is 2", () => {
    expect(Object.create).toHaveLength(2);
});

describe("errors", () => {
    test("non-object prototype value", () => {
        expect(() => Object.create(42)).toThrowWithMessage(
            TypeError,
            "Prototype must be an object or null"
        );
    });
});

describe("normal behavior", () => {
    test("creates object with given prototype", () => {
        let o;

        o = Object.create(null);
        expect(o).toEqual({});
        expect(Object.getPrototypeOf(o)).toBe(null);

        const p = {};
        o = Object.create(p);
        expect(o).toEqual({});
        expect(Object.getPrototypeOf(o)).toBe(p);
    });

    test("creates object with properties from propertiesObject, if given", () => {
        const o = Object.create(
            {},
            {
                foo: {
                    writable: true,
                    configurable: true,
                    value: "foo",
                },
                bar: {
                    enumerable: true,
                    value: "bar",
                },
            }
        );
        expect(Object.getOwnPropertyNames(o)).toEqual(["foo", "bar"]);
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
    });
});
