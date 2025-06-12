test("length is 1", () => {
    expect(Reflect.isExtensible).toHaveLength(1);
});

describe("errors", () => {
    test("target must be an object", () => {
        [null, undefined, "foo", 123, NaN, Infinity].forEach(value => {
            expect(() => {
                Reflect.isExtensible(value);
            }).toThrowWithMessage(TypeError, `${value} is not an object`);
        });
    });
});

describe("normal behavior", () => {
    test("regular object is extensible", () => {
        expect(Reflect.isExtensible({})).toBeTrue();
    });

    test("global object is extensible", () => {
        expect(Reflect.isExtensible(globalThis)).toBeTrue();
    });

    test("regular object is not extensible after preventExtensions()", () => {
        var o = {};
        expect(Reflect.isExtensible(o)).toBeTrue();
        Reflect.preventExtensions(o);
        expect(Reflect.isExtensible(o)).toBeFalse();
    });
});
