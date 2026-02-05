describe("[[PreventExtension]] trap normal behavior", () => {
    test("forwarding when not defined in handler", () => {
        let p = new Proxy({}, { preventExtensions: null });
        expect(Object.preventExtensions(p)).toBe(p);
        p = new Proxy({}, { preventExtensions: undefined });
        expect(Object.preventExtensions(p)).toBe(p);
        p = new Proxy({}, {});
        expect(Object.preventExtensions(p)).toBe(p);
    });

    test("correct arguments supplied to trap", () => {
        let o = {};
        p = new Proxy(o, {
            preventExtensions(target) {
                expect(target).toBe(o);
                return true;
            },
        });

        Object.preventExtensions(o);
        Object.preventExtensions(p);
    });
});

describe("[[PreventExtensions]] invariants", () => {
    test("cannot return false", () => {
        let p = new Proxy(
            {},
            {
                preventExtensions() {
                    return false;
                },
            }
        );

        expect(() => {
            Object.preventExtensions(p);
        }).toThrowWithMessage(TypeError, "Object's [[PreventExtensions]] method returned false");
    });

    test("cannot return true if the target is extensible", () => {
        let o = {};
        let p = new Proxy(o, {
            preventExtensions() {
                return true;
            },
        });

        expect(() => {
            Object.preventExtensions(p);
        }).toThrowWithMessage(
            TypeError,
            "Proxy handler's preventExtensions trap violates invariant: cannot return true if the target object is extensible"
        );

        Object.preventExtensions(o);
        expect(Object.preventExtensions(p)).toBe(p);
    });
});
