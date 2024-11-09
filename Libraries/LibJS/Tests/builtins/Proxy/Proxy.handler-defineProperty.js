describe("[[DefineProperty]] trap normal behavior", () => {
    test("forwarding when not defined in handler", () => {
        let p = new Proxy({}, { defineProperty: null });
        expect(Object.defineProperty(p, "foo", {})).toBe(p);
        p = new Proxy({}, { defineProperty: undefined });
        expect(Object.defineProperty(p, "foo", {})).toBe(p);
        p = new Proxy({}, {});
        expect(Object.defineProperty(p, "foo", {})).toBe(p);
    });

    test("correct arguments passed to trap", () => {
        let o = {};
        p = new Proxy(o, {
            defineProperty(target, name, descriptor) {
                expect(target).toBe(o);
                expect(name).toBe("foo");
                expect(descriptor.configurable).toBeTrue();
                expect(descriptor.enumerable).toBeUndefined();
                expect(descriptor.writable).toBeTrue();
                expect(descriptor.value).toBe(10);
                expect(descriptor.get).toBeUndefined();
                expect(descriptor.set).toBeUndefined();
                return true;
            },
        });

        Object.defineProperty(p, "foo", { configurable: true, writable: true, value: 10 });
    });

    test("correct arguments passed to trap even for number", () => {
        let o = {};
        p = new Proxy(o, {
            defineProperty(target, name, descriptor) {
                expect(target).toBe(o);
                expect(name).toBe("1");
                expect(descriptor.configurable).toBeTrue();
                expect(descriptor.enumerable).toBeUndefined();
                expect(descriptor.writable).toBeTrue();
                expect(descriptor.value).toBe(10);
                expect(descriptor.get).toBeUndefined();
                expect(descriptor.set).toBeUndefined();
                return true;
            },
        });

        Object.defineProperty(p, 1, { configurable: true, writable: true, value: 10 });
    });

    test("optionally ignoring the define call", () => {
        let o = {};
        let p = new Proxy(o, {
            defineProperty(target, name, descriptor) {
                if (target[name] === undefined) Object.defineProperty(target, name, descriptor);
                return true;
            },
        });

        Object.defineProperty(p, "foo", {
            value: 10,
            enumerable: true,
            configurable: false,
            writable: true,
        });
        expect(p).toHaveEnumerableProperty("foo");
        expect(p).not.toHaveConfigurableProperty("foo");
        expect(p).toHaveWritableProperty("foo");
        expect(p).toHaveValueProperty("foo", 10);
        expect(p).not.toHaveGetterProperty("foo");
        expect(p).not.toHaveSetterProperty("foo");

        Object.defineProperty(p, "foo", {
            value: 20,
            enumerable: true,
            configurable: false,
            writable: true,
        });
        expect(p).toHaveEnumerableProperty("foo");
        expect(p).not.toHaveConfigurableProperty("foo");
        expect(p).toHaveWritableProperty("foo");
        expect(p).toHaveValueProperty("foo", 10);
        expect(p).not.toHaveGetterProperty("foo");
        expect(p).not.toHaveSetterProperty("foo");
    });
});

describe("[[DefineProperty]] invariants", () => {
    test("trap can't return false", () => {
        let p = new Proxy(
            {},
            {
                defineProperty() {
                    return false;
                },
            }
        );

        expect(() => {
            Object.defineProperty(p, "foo", {});
        }).toThrowWithMessage(TypeError, "Object's [[DefineOwnProperty]] method returned false");
    });

    test("trap cannot return true for a non-extensible target if the property does not exist", () => {
        let o = {};
        Object.preventExtensions(o);
        let p = new Proxy(o, {
            defineProperty() {
                return true;
            },
        });

        expect(() => {
            Object.defineProperty(p, "foo", {});
        }).toThrowWithMessage(
            TypeError,
            "Proxy handler's defineProperty trap violates invariant: a property cannot be reported as being defined if the property does not exist on the target and the target is non-extensible"
        );
    });

    test("trap cannot return true for a non-configurable property if it doesn't already exist on the target", () => {
        let o = {};
        Object.defineProperty(o, "foo", { value: 10, configurable: true });
        let p = new Proxy(o, {
            defineProperty() {
                return true;
            },
        });

        expect(() => {
            Object.defineProperty(p, "bar", { value: 6, configurable: false });
        }).toThrowWithMessage(
            TypeError,
            "Proxy handler's defineProperty trap violates invariant: a property cannot be defined as non-configurable if it does not already exist on the target object"
        );
    });

    test("trap cannot return true for a non-configurable property if it already exists as a configurable property", () => {
        let o = {};
        Object.defineProperty(o, "foo", { value: 10, configurable: true });
        let p = new Proxy(o, {
            defineProperty() {
                return true;
            },
        });

        expect(() => {
            Object.defineProperty(p, "foo", { value: 6, configurable: false });
        }).toThrowWithMessage(
            TypeError,
            "Proxy handler's defineProperty trap violates invariant: a property cannot be defined as non-configurable if it already exists on the target object as a configurable property"
        );
    });
});
