describe("normal functionality", () => {
    let s = Symbol("foo");

    test("non-configurable string property", () => {
        let o = {};
        Object.defineProperty(o, "foo", { value: 1, writable: false, enumerable: false });

        expect(o.foo).toBe(1);
        o.foo = 2;
        expect(o.foo).toBe(1);

        expect(o).not.toHaveConfigurableProperty("foo");
        expect(o).not.toHaveEnumerableProperty("foo");
        expect(o).not.toHaveWritableProperty("foo");
        expect(o).toHaveValueProperty("foo", 1);
    });

    test("non-configurable symbol property", () => {
        let o = {};
        Object.defineProperty(o, s, { value: 1, writable: false, enumerable: false });

        expect(o[s]).toBe(1);
        o[s] = 2;
        expect(o[s]).toBe(1);

        expect(o).not.toHaveConfigurableProperty(s);
        expect(o).not.toHaveEnumerableProperty(s);
        expect(o).not.toHaveWritableProperty(s);
        expect(o).toHaveValueProperty(s, 1);
    });

    test("array index getter", () => {
        let o = {};
        Object.defineProperty(o, 2, {
            get() {
                return 10;
            },
        });
        expect(o[2]).toBe(10);
    });

    test("symbol property getter", () => {
        let o = {};
        Object.defineProperty(o, s, {
            get() {
                return 10;
            },
        });
        expect(o[s]).toBe(10);
    });

    test("configurable string property", () => {
        let o = {};
        Object.defineProperty(o, "foo", { value: "hi", writable: true, enumerable: true });

        expect(o.foo).toBe("hi");
        o.foo = "ho";
        expect(o.foo).toBe("ho");

        expect(o).not.toHaveConfigurableProperty("foo");
        expect(o).toHaveEnumerableProperty("foo");
        expect(o).toHaveWritableProperty("foo");
        expect(o).toHaveValueProperty("foo", "ho");
    });

    test("configurable symbol property", () => {
        let o = {};
        Object.defineProperty(o, s, { value: "hi", writable: true, enumerable: true });

        expect(o[s]).toBe("hi");
        o[s] = "ho";
        expect(o[s]).toBe("ho");

        expect(o).not.toHaveConfigurableProperty(s);
        expect(o).toHaveEnumerableProperty(s);
        expect(o).toHaveWritableProperty(s);
        expect(o).toHaveValueProperty(s, "ho");
    });

    test("reconfigure configurable string property", () => {
        let o = {};
        Object.defineProperty(o, "foo", { value: 9, configurable: true, writable: false });
        Object.defineProperty(o, "foo", { configurable: true, writable: true });

        expect(o).toHaveConfigurableProperty("foo");
        expect(o).toHaveWritableProperty("foo");
        expect(o).not.toHaveEnumerableProperty("foo");
        expect(o).toHaveValueProperty("foo", 9);
    });

    test("reconfigure configurable symbol property", () => {
        let o = {};
        Object.defineProperty(o, s, { value: 9, configurable: true, writable: false });
        Object.defineProperty(o, s, { configurable: true, writable: true });

        expect(o).toHaveConfigurableProperty(s);
        expect(o).toHaveWritableProperty(s);
        expect(o).not.toHaveEnumerableProperty(s);
        expect(o).toHaveValueProperty(s, 9);
    });

    test("define string accessor", () => {
        let o = {};

        Object.defineProperty(o, "foo", {
            configurable: true,
            get() {
                return o.secret_foo + 1;
            },
            set(value) {
                this.secret_foo = value + 1;
            },
        });

        o.foo = 10;
        expect(o.foo).toBe(12);
        o.foo = 20;
        expect(o.foo).toBe(22);

        Object.defineProperty(o, "foo", { configurable: true, value: 4 });

        expect(o.foo).toBe(4);
        expect((o.foo = 5)).toBe(5);
        expect((o.foo = 4)).toBe(4);
    });

    test("define symbol accessor", () => {
        let o = {};

        Object.defineProperty(o, s, {
            configurable: true,
            get() {
                return o.secret_foo + 1;
            },
            set(value) {
                this.secret_foo = value + 1;
            },
        });

        o[s] = 10;
        expect(o[s]).toBe(12);
        o[s] = 20;
        expect(o[s]).toBe(22);

        Object.defineProperty(o, s, { configurable: true, value: 4 });

        expect(o[s]).toBe(4);
        expect((o[s] = 5)).toBe(5);
        expect((o[s] = 4)).toBe(4);
    });

    test("issue #3735, reconfiguring property in unique shape", () => {
        const o = {};
        // In LibJS an object with more than 100 properties gets a unique shape
        for (let i = 0; i < 101; ++i) {
            o[`property${i}`] = i;
        }
        Object.defineProperty(o, "x", { configurable: true });
        Object.defineProperty(o, "x", { configurable: false });
    });
});

describe("errors", () => {
    test("redefine non-configurable property", () => {
        let o = {};
        Object.defineProperty(o, "foo", { value: 1, writable: true, enumerable: true });

        expect(() => {
            Object.defineProperty(o, "foo", { value: 2, writable: true, enumerable: false });
        }).toThrowWithMessage(TypeError, "Object's [[DefineOwnProperty]] method returned false");
    });

    test("redefine non-configurable symbol property", () => {
        let o = {};
        let s = Symbol("foo");
        Object.defineProperty(o, s, { value: 1, writable: true, enumerable: true });

        expect(() => {
            Object.defineProperty(o, s, { value: 2, writable: true, enumerable: false });
        }).toThrowWithMessage(TypeError, "Object's [[DefineOwnProperty]] method returned false");
    });

    test("cannot define 'value' and 'get' in the same descriptor", () => {
        let o = {};

        expect(() => {
            Object.defineProperty(o, "a", {
                get() {},
                value: 9,
            });
        }).toThrowWithMessage(
            TypeError,
            "Accessor property descriptor cannot specify a value or writable key"
        );
    });

    test("cannot define 'value' and 'set' in the same descriptor", () => {
        let o = {};

        expect(() => {
            Object.defineProperty(o, "a", {
                set() {},
                writable: true,
            });
        }).toThrowWithMessage(
            TypeError,
            "Accessor property descriptor cannot specify a value or writable key"
        );
    });

    test("redefine non-configurable accessor", () => {
        let o = {};

        Object.defineProperty(o, "foo", {
            configurable: false,
            get() {
                return this.secret_foo + 2;
            },
            set(value) {
                o.secret_foo = value + 2;
            },
        });

        expect(() => {
            Object.defineProperty(o, "foo", {
                configurable: false,
                get() {
                    return this.secret_foo + 2;
                },
            });
        }).toThrowWithMessage(TypeError, "Object's [[DefineOwnProperty]] method returned false");
    });
});
