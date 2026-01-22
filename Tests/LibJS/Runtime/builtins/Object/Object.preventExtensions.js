describe("correct behavior", () => {
    test("non-object arguments", () => {
        expect(Object.preventExtensions()).toBeUndefined();
        expect(Object.preventExtensions(undefined)).toBeUndefined();
        expect(Object.preventExtensions(null)).toBeNull();
        expect(Object.preventExtensions(true)).toBeTrue();
        expect(Object.preventExtensions(6)).toBe(6);
        expect(Object.preventExtensions("test")).toBe("test");

        let s = Symbol();
        expect(Object.preventExtensions(s)).toBe(s);
    });

    test("basic functionality", () => {
        let o = { foo: "foo" };
        expect(o.foo).toBe("foo");
        o.bar = "bar";
        expect(o.bar).toBe("bar");

        expect(Object.preventExtensions(o)).toBe(o);
        expect(o.foo).toBe("foo");
        expect(o.bar).toBe("bar");

        o.baz = "baz";
        expect(o.baz).toBeUndefined();
    });

    test("modifying existing properties", () => {
        const o = { foo: "bar" };
        Object.preventExtensions(o);
        o.foo = "baz";
        expect(o.foo).toBe("baz");
    });

    test("deleting existing properties", () => {
        const o = { foo: "bar" };
        Object.preventExtensions(o);
        delete o.foo;
        expect(o).not.toHaveProperty("foo");
    });
});

describe("errors", () => {
    test("defining a property on a non-extensible object", () => {
        let o = {};
        Object.preventExtensions(o);

        expect(() => {
            Object.defineProperty(o, "baz", { value: "baz" });
        }).toThrowWithMessage(TypeError, "Object's [[DefineOwnProperty]] method returned false");

        expect(o.baz).toBeUndefined();
    });

    test("putting property on a non-extensible object", () => {
        let o = {};
        Object.preventExtensions(o);

        expect(() => {
            "use strict";
            o.foo = "foo";
        }).toThrowWithMessage(TypeError, "Cannot set property 'foo' of [object Object]");

        expect((o.foo = "foo")).toBe("foo");
        expect(o.foo).toBeUndefined();
    });
});
