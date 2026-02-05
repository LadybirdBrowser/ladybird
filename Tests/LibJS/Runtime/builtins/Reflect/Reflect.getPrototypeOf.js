test("length is 1", () => {
    expect(Reflect.getPrototypeOf).toHaveLength(1);
});

describe("errors", () => {
    test("target must be an object", () => {
        [null, undefined, "foo", 123, NaN, Infinity].forEach(value => {
            expect(() => {
                Reflect.getPrototypeOf(value);
            }).toThrowWithMessage(TypeError, `${value} is not an object`);
        });
    });
});

describe("normal behavior", () => {
    test("get prototype of regular object", () => {
        expect(Reflect.getPrototypeOf({})).toBe(Object.prototype);
    });

    test("get prototype of array", () => {
        expect(Reflect.getPrototypeOf([])).toBe(Array.prototype);
    });

    test("get prototype of string object", () => {
        expect(Reflect.getPrototypeOf(new String())).toBe(String.prototype);
    });

    test("get user-defined prototype of regular object", () => {
        var o = {};
        var p = { foo: "bar" };
        Reflect.setPrototypeOf(o, p);
        expect(Reflect.getPrototypeOf(o)).toBe(p);
    });
});
