test("length is 1", () => {
    expect(Object.seal).toHaveLength(1);
});

describe("normal behavior", () => {
    test("returns given argument", () => {
        const o = {};
        expect(Object.seal(42)).toBe(42);
        expect(Object.seal("foobar")).toBe("foobar");
        expect(Object.seal(o)).toBe(o);
    });

    test("prevents addition of new properties", () => {
        const o = {};
        expect(o.foo).toBeUndefined();
        Object.seal(o);
        o.foo = "bar";
        expect(o.foo).toBeUndefined();
    });

    test("prevents deletion of existing properties", () => {
        const o = { foo: "bar" };
        expect(o.foo).toBe("bar");
        Object.seal(o);
        delete o.foo;
        expect(o.foo).toBe("bar");
    });

    test("prevents changing attributes of existing properties", () => {
        const o = { foo: "bar" };
        Object.seal(o);
        expect(Object.defineProperty(o, "foo", {})).toBe(o);
        expect(Object.defineProperty(o, "foo", { configurable: false })).toBe(o);
        expect(() => {
            Object.defineProperty(o, "foo", { configurable: true });
        }).toThrowWithMessage(TypeError, "Object's [[DefineOwnProperty]] method returned false");
    });

    test("doesn't prevent changing value of existing properties", () => {
        const o = { foo: "bar" };
        expect(o.foo).toBe("bar");
        Object.seal(o);
        o.foo = "baz";
        expect(o.foo).toBe("baz");
    });

    // #6469
    test("works with indexed properties", () => {
        const a = ["foo"];
        expect(a[0]).toBe("foo");
        Object.seal(a);
        a[0] = "bar";
        a[1] = "baz";
        expect(a[0]).toBe("bar");
        expect(a[1]).toBeUndefined();
    });

    test("works with properties that are already non-configurable", () => {
        const o = {};
        Object.defineProperty(o, "foo", {
            value: "bar",
            configurable: false,
            writable: true,
            enumerable: true,
        });
        expect(o.foo).toBe("bar");
        Object.seal(o);
        o.foo = "baz";
        expect(o.foo).toBe("baz");
    });
});
