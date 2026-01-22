test("length is 2", () => {
    expect(Reflect.getOwnPropertyDescriptor).toHaveLength(2);
});

describe("errors", () => {
    test("target must be an object", () => {
        [null, undefined, "foo", 123, NaN, Infinity].forEach(value => {
            expect(() => {
                Reflect.getOwnPropertyDescriptor(value);
            }).toThrowWithMessage(TypeError, `${value} is not an object`);
        });
    });
});

describe("normal behavior", () => {
    test("get descriptor of undefined object property", () => {
        expect(Reflect.getOwnPropertyDescriptor({})).toBeUndefined();
        expect(Reflect.getOwnPropertyDescriptor({}, "foo")).toBeUndefined();
    });

    test("get descriptor of defined object property", () => {
        var o = { foo: "bar" };
        var d = Reflect.getOwnPropertyDescriptor(o, "foo");
        expect(d.value).toBe("bar");
        expect(d.writable).toBeTrue();
        expect(d.enumerable).toBeTrue();
        expect(d.configurable).toBeTrue();
    });

    test("get descriptor of array length property", () => {
        var a = [];
        d = Reflect.getOwnPropertyDescriptor(a, "length");
        expect(d.value).toBe(0);
        expect(d.writable).toBeTrue();
        expect(d.enumerable).toBeFalse();
        expect(d.configurable).toBeFalse();
    });
});
