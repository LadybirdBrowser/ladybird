test("length is 2", () => {
    expect(Reflect.deleteProperty).toHaveLength(2);
});

describe("errors", () => {
    test("target must be an object", () => {
        [null, undefined, "foo", 123, NaN, Infinity].forEach(value => {
            expect(() => {
                Reflect.deleteProperty(value);
            }).toThrowWithMessage(TypeError, `${value} is not an object`);
        });
    });
});

describe("normal behavior", () => {
    test("deleting non-existent property", () => {
        expect(Reflect.deleteProperty({})).toBeTrue();
        expect(Reflect.deleteProperty({}, "foo")).toBeTrue();
    });

    test("deleting existent property", () => {
        var o = { foo: 1 };
        expect(o.foo).toBe(1);
        expect(Reflect.deleteProperty(o, "foo")).toBeTrue();
        expect(o.foo).toBeUndefined();
        expect(Reflect.deleteProperty(o, "foo")).toBeTrue();
        expect(o.foo).toBeUndefined();
    });

    test("deleting existent, configurable, non-writable property", () => {
        var o = {};
        Object.defineProperty(o, "foo", { value: 1, configurable: true, writable: false });
        expect(Reflect.deleteProperty(o, "foo")).toBeTrue();
        expect(o.foo).toBeUndefined();
    });

    test("deleting existent, non-configurable, writable property", () => {
        var o = {};
        Object.defineProperty(o, "foo", { value: 1, configurable: false, writable: true });
        expect(Reflect.deleteProperty(o, "foo")).toBeFalse();
        expect(o.foo).toBe(1);
    });

    test("deleting existent, non-configurable, non-writable property", () => {
        var o = {};
        Object.defineProperty(o, "foo", { value: 1, configurable: false, writable: false });
        expect(Reflect.deleteProperty(o, "foo")).toBeFalse();
        expect(o.foo).toBe(1);
    });

    test("deleting array index", () => {
        var a = [1, 2, 3];
        expect(a).toHaveLength(3);
        expect(a[0]).toBe(1);
        expect(a[1]).toBe(2);
        expect(a[2]).toBe(3);
        expect(Reflect.deleteProperty(a, 1)).toBeTrue();
        expect(a).toHaveLength(3);
        expect(a[0]).toBe(1);
        expect(a[1]).toBeUndefined();
        expect(a[2]).toBe(3);
    });
});
