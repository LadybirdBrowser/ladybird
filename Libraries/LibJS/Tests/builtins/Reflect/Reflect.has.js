test("length is 2", () => {
    expect(Reflect.has).toHaveLength(2);
});

describe("errors", () => {
    test("target must be an object", () => {
        [null, undefined, "foo", 123, NaN, Infinity].forEach(value => {
            expect(() => {
                Reflect.has(value);
            }).toThrowWithMessage(TypeError, `${value} is not an object`);
        });
    });
});

describe("normal behavior", () => {
    test("regular object has property", () => {
        expect(Reflect.has({})).toBeFalse();
        expect(Reflect.has({ undefined })).toBeTrue();
        expect(Reflect.has({ 0: "1" }, "0")).toBeTrue();
        expect(Reflect.has({ foo: "bar" }, "foo")).toBeTrue();
        expect(Reflect.has({ bar: "baz" }, "foo")).toBeFalse();
        expect(Reflect.has({}, "toString")).toBeTrue();
    });

    test("array has property", () => {
        expect(Reflect.has([])).toBeFalse();
        expect(Reflect.has([], 0)).toBeFalse();
        expect(Reflect.has([1, 2, 3], "0")).toBeTrue();
        expect(Reflect.has([1, 2, 3], 0)).toBeTrue();
        expect(Reflect.has([1, 2, 3], 1)).toBeTrue();
        expect(Reflect.has([1, 2, 3], 2)).toBeTrue();
        expect(Reflect.has([1, 2, 3], 3)).toBeFalse();
        expect(Reflect.has([], "pop")).toBeTrue();
    });

    test("string object has property", () => {
        expect(Reflect.has(new String())).toBeFalse();
        expect(Reflect.has(new String("foo"), "0")).toBeTrue();
        expect(Reflect.has(new String("foo"), 0)).toBeTrue();
        expect(Reflect.has(new String("foo"), 1)).toBeTrue();
        expect(Reflect.has(new String("foo"), 2)).toBeTrue();
        expect(Reflect.has(new String("foo"), 3)).toBeFalse();
        expect(Reflect.has(new String("foo"), "charAt")).toBeTrue();
    });
});
