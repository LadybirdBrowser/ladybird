test("length is 1", () => {
    expect(Object.freeze).toHaveLength(1);
});

describe("normal behavior", () => {
    test("returns given argument", () => {
        const o = {};
        expect(Object.freeze(42)).toBe(42);
        expect(Object.freeze("foobar")).toBe("foobar");
        expect(Object.freeze(o)).toBe(o);
    });

    test("prevents addition of new properties", () => {
        const o = {};
        expect(o.foo).toBeUndefined();
        Object.freeze(o);
        o.foo = "bar";
        expect(o.foo).toBeUndefined();
    });

    test("prevents deletion of existing properties", () => {
        const o = { foo: "bar" };
        expect(o.foo).toBe("bar");
        Object.freeze(o);
        delete o.foo;
        expect(o.foo).toBe("bar");
    });

    test("prevents changing attributes of existing properties", () => {
        const o = { foo: "bar" };
        Object.freeze(o);
        expect(Object.defineProperty(o, "foo", {})).toBe(o);
        expect(Object.defineProperty(o, "foo", { configurable: false })).toBe(o);
        expect(() => {
            Object.defineProperty(o, "foo", { configurable: true });
        }).toThrowWithMessage(TypeError, "Object's [[DefineOwnProperty]] method returned false");
    });

    test("prevents changing value of existing properties", () => {
        const o = { foo: "bar" };
        expect(o.foo).toBe("bar");
        Object.freeze(o);
        o.foo = "baz";
        expect(o.foo).toBe("bar");
    });

    // #6469
    test("works with indexed properties", () => {
        const a = ["foo"];
        expect(a[0]).toBe("foo");
        Object.freeze(a);
        a[0] = "bar";
        expect(a[0]).toBe("foo");
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
        Object.freeze(o);
        o.foo = "baz";
        expect(o.foo).toBe("bar");
    });
});

test("does not override frozen function name", () => {
    const func = Object.freeze(function () {
        return 12;
    });
    const obj = Object.freeze({ name: func });
    expect(obj.name()).toBe(12);
});

test("freeze with huge number of properties doesn't crash", () => {
    const o = {};
    for (let i = 0; i < 50_000; ++i) {
        o["prop" + i] = 1;
    }
    Object.freeze(o);
});
