test("length is 3", () => {
    expect(Reflect.defineProperty).toHaveLength(3);
});

describe("errors", () => {
    test("target must be an object", () => {
        [null, undefined, "foo", 123, NaN, Infinity].forEach(value => {
            expect(() => {
                Reflect.defineProperty(value);
            }).toThrowWithMessage(TypeError, `${value} is not an object`);
        });
    });

    test("descriptor must be an object", () => {
        [null, undefined, "foo", 123, NaN, Infinity].forEach(value => {
            expect(() => {
                Reflect.defineProperty({}, "foo", value);
            }).toThrowWithMessage(TypeError, `${value} is not an object`);
        });
    });
});

describe("normal behavior", () => {
    test("initial value and non-writable", () => {
        var o = {};

        expect(o.foo).toBeUndefined();
        expect(Reflect.defineProperty(o, "foo", { value: 1, writable: false })).toBeTrue();
        expect(o.foo).toBe(1);
        o.foo = 2;
        expect(o.foo).toBe(1);
    });

    test("initial value and writable", () => {
        var o = {};

        expect(o.foo).toBeUndefined();
        expect(Reflect.defineProperty(o, "foo", { value: 1, writable: true })).toBeTrue();
        expect(o.foo).toBe(1);
        o.foo = 2;
        expect(o.foo).toBe(2);
    });

    test("can redefine value of configurable, writable property", () => {
        var o = {};

        expect(o.foo).toBeUndefined();
        expect(
            Reflect.defineProperty(o, "foo", { value: 1, configurable: true, writable: true })
        ).toBeTrue();
        expect(o.foo).toBe(1);
        expect(Reflect.defineProperty(o, "foo", { value: 2 })).toBeTrue();
        expect(o.foo).toBe(2);
    });

    test("can redefine value of configurable, non-writable property", () => {
        var o = {};

        expect(o.foo).toBeUndefined();
        expect(
            Reflect.defineProperty(o, "foo", { value: 1, configurable: true, writable: false })
        ).toBeTrue();
        expect(o.foo).toBe(1);
        expect(Reflect.defineProperty(o, "foo", { value: 2 })).toBeTrue();
        expect(o.foo).toBe(2);
    });

    test("cannot redefine value of non-configurable, non-writable property", () => {
        var o = {};

        expect(o.foo).toBeUndefined();
        expect(
            Reflect.defineProperty(o, "foo", { value: 1, configurable: false, writable: false })
        ).toBeTrue();
        expect(o.foo).toBe(1);
        expect(Reflect.defineProperty(o, "foo", { value: 2 })).toBeFalse();
        expect(o.foo).toBe(1);
    });
});
