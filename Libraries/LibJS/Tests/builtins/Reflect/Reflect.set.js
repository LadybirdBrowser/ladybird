test("length is 3", () => {
    expect(Reflect.set).toHaveLength(3);
});

describe("errors", () => {
    test("target must be an object", () => {
        [null, undefined, "foo", 123, NaN, Infinity].forEach(value => {
            expect(() => {
                Reflect.set(value);
            }).toThrowWithMessage(TypeError, `${value} is not an object`);
        });
    });
});

describe("normal behavior", () => {
    test("setting properties of regular object", () => {
        var o = {};

        expect(Reflect.set(o)).toBeTrue();
        expect(o.undefined).toBeUndefined();

        expect(Reflect.set(o, "foo")).toBeTrue();
        expect(o.foo).toBeUndefined();

        expect(Reflect.set(o, "foo", "bar")).toBeTrue();
        expect(o.foo).toBe("bar");

        expect(Reflect.set(o, "foo", 42)).toBeTrue();
        expect(o.foo).toBe(42);
    });

    test("setting configurable, non-writable property of regular object", () => {
        var o = {};
        Object.defineProperty(o, "foo", { value: 1, configurable: true, writable: false });
        expect(Reflect.set(o, "foo", 2)).toBeFalse();
        expect(o.foo).toBe(1);
    });

    test("setting non-configurable, writable property of regular object", () => {
        var o = {};
        Object.defineProperty(o, "foo", { value: 1, configurable: false, writable: true });
        expect(Reflect.set(o, "foo", 2)).toBeTrue();
        expect(o.foo).toBe(2);
    });

    test("", () => {
        var a = [];
        expect(a.length === 0);
        expect(Reflect.set(a, "0")).toBeTrue();
        expect(a.length === 1);
        expect(a[0]).toBeUndefined();
        expect(Reflect.set(a, 1, "foo")).toBeTrue();
        expect(a.length === 2);
        expect(a[0]).toBeUndefined();
        expect(a[1] === "foo");
        expect(Reflect.set(a, 4, "bar")).toBeTrue();
        expect(a.length === 5);
        expect(a[0]).toBeUndefined();
        expect(a[1] === "foo");
        expect(a[2]).toBeUndefined();
        expect(a[3]).toBeUndefined();
        expect(a[4] === "bar");
    });

    test("setting setter property of regular object", () => {
        const foo = {
            set prop(value) {
                this.setPropCalled = true;
            },
        };
        expect(foo.setPropCalled).toBeUndefined();
        Reflect.set(foo, "prop", 42);
        expect(foo.setPropCalled).toBeTrue();
    });

    test("setting setter property of regular object with different receiver", () => {
        const foo = {
            set prop(value) {
                this.setPropCalled = true;
            },
        };
        const bar = {};
        Object.setPrototypeOf(bar, foo);

        expect(foo.setPropCalled).toBeUndefined();
        expect(bar.setPropCalled).toBeUndefined();

        Reflect.set(bar, "prop", 42);
        expect(foo.setPropCalled).toBeUndefined();
        expect(bar.setPropCalled).toBeTrue();

        Reflect.set(bar, "prop", 42, foo);
        expect(foo.setPropCalled).toBeTrue();
        expect(bar.setPropCalled).toBeTrue();
    });

    test("native setter function", () => {
        const e = new Error();
        Reflect.set(Error.prototype, "name", "Foo", e);
        expect(e.name).toBe("Foo");
    });
});
