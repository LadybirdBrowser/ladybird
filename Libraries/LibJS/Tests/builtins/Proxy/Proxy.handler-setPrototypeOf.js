describe("[[SetPrototypeOf]] trap normal behavior", () => {
    test("forwarding when not defined in handler", () => {
        const o = {};
        const proto = { foo: "bar" };
        Object.setPrototypeOf(o, proto);

        let p = new Proxy(o, { setPrototypeOf: null });
        expect(Object.setPrototypeOf(p, proto)).toBe(p);
        p = new Proxy(o, { setPrototypeOf: undefined });
        expect(Object.setPrototypeOf(p, proto)).toBe(p);
        p = new Proxy(o, {});
        expect(Object.setPrototypeOf(p, proto)).toBe(p);
    });

    test("correct arguments supplied to trap", () => {
        let o = {};
        let theNewProto = { foo: "bar" };

        let p = new Proxy(o, {
            setPrototypeOf(target, newProto) {
                expect(target).toBe(o);
                expect(newProto).toBe(theNewProto);
                return true;
            },
        });

        Object.setPrototypeOf(p, theNewProto);
    });

    test("conditional setting", () => {
        let o = {};

        let p = new Proxy(o, {
            setPrototypeOf(target, newProto) {
                if (target.shouldSet) Object.setPrototypeOf(target, newProto);
                return true;
            },
        });

        Object.setPrototypeOf(p, { foo: 1 });
        expect(Object.getPrototypeOf(p).foo).toBeUndefined();
        p.shouldSet = true;
        expect(o.shouldSet).toBeTrue();
        Object.setPrototypeOf(p, { foo: 1 });
        expect(Object.getPrototypeOf(p).foo).toBe(1);
    });

    test("non-extensible targets", () => {
        let o = {};
        let proto = {};
        Object.setPrototypeOf(o, proto);
        Object.preventExtensions(o);

        p = new Proxy(o, {
            setPrototypeOf() {
                return true;
            },
        });

        expect(Object.setPrototypeOf(p, proto)).toBe(p);
        expect(Object.getPrototypeOf(p)).toBe(proto);
    });
});

describe("[[SetPrototypeOf]] invariants", () => {
    test("cannot return false", () => {
        let o = {};
        p = new Proxy(o, {
            setPrototypeOf() {
                return false;
            },
        });

        expect(() => {
            Object.setPrototypeOf(p, {});
        }).toThrowWithMessage(TypeError, "Object's [[SetPrototypeOf]] method returned false");
    });

    test("the argument must match the target's prototype if the target is non-extensible", () => {
        let o = {};
        Object.preventExtensions(o);

        let p = new Proxy(o, {
            setPrototypeOf() {
                return true;
            },
        });

        expect(() => {
            Object.setPrototypeOf(p, {});
        }).toThrowWithMessage(
            TypeError,
            "Proxy handler's setPrototypeOf trap violates invariant: the argument must match the prototype of the target if the target is non-extensible"
        );
    });
});
