describe("[[GetPrototypeOf]] trap normal behavior", () => {
    test("forwarding when not defined in handler", () => {
        let proto = {};
        let o = {};
        Object.setPrototypeOf(o, proto);

        let p = new Proxy(o, { prototype: null });
        expect(Object.getPrototypeOf(p)).toBe(proto);
        p = new Proxy(o, { apply: undefined });
        expect(Object.getPrototypeOf(p)).toBe(proto);
        p = new Proxy(o, {});
        expect(Object.getPrototypeOf(p)).toBe(proto);
    });

    test("correct arguments supplied to trap", () => {
        let o = {};
        let p = new Proxy(o, {
            getPrototypeOf(target) {
                expect(target).toBe(o);
                return null;
            },
        });

        Object.getPrototypeOf(p);
    });

    test("conditional return", () => {
        let o = {};
        let p = new Proxy(o, {
            getPrototypeOf(target) {
                if (target.foo) return { bar: 1 };
                return { bar: 2 };
            },
        });

        expect(Object.getPrototypeOf(p).bar).toBe(2);
        o.foo = 20;
        expect(Object.getPrototypeOf(p).bar).toBe(1);
    });

    test("non-extensible target", () => {
        let o = {};
        let proto = { foo: "bar" };
        Object.setPrototypeOf(o, proto);
        Object.preventExtensions(o);

        p = new Proxy(o, {
            getPrototypeOf() {
                return proto;
            },
        });

        expect(Object.getPrototypeOf(p).foo).toBe("bar");
    });
});

describe("[[GetPrototypeOf]] invariants", () => {
    test("must return an object or null", () => {
        expect(() => {
            Object.getPrototypeOf(
                new Proxy(
                    {},
                    {
                        getPrototypeOf() {
                            return 1;
                        },
                    }
                )
            );
        }).toThrowWithMessage(
            TypeError,
            "Proxy handler's getPrototypeOf trap violates invariant: must return an object or null"
        );
    });

    test("different return object for non-extensible target", () => {
        let o = {};
        let proto = {};
        Object.setPrototypeOf(o, proto);
        Object.preventExtensions(o);

        let p = new Proxy(o, {
            getPrototypeOf(target) {
                return { baz: "qux" };
            },
        });

        expect(() => {
            Object.getPrototypeOf(p);
        }).toThrowWithMessage(
            TypeError,
            "Proxy handler's getPrototypeOf trap violates invariant: cannot return a different prototype object for a non-extensible target"
        );
    });
});
