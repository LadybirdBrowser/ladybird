describe("correct behavior", () => {
    test("length", () => {
        expect(Object.keys).toHaveLength(1);
        expect(Object.keys(true)).toHaveLength(0);
        expect(Object.keys(45)).toHaveLength(0);
        expect(Object.keys(-998)).toHaveLength(0);
        expect(Object.keys("abcd")).toHaveLength(4);
        expect(Object.keys([1, 2, 3])).toHaveLength(3);
        expect(Object.keys({ a: 1, b: 2, c: 3 })).toHaveLength(3);
    });

    test("object argument", () => {
        let keys = Object.keys({ foo: 1, bar: 2, baz: 3 });
        expect(keys).toEqual(["foo", "bar", "baz"]);
    });

    test("object argument with symbol keys", () => {
        let keys = Object.keys({ foo: 1, [Symbol("bar")]: 2, baz: 3 });
        expect(keys).toEqual(["foo", "baz"]);
    });

    test("array argument", () => {
        let keys = Object.keys(["a", "b", "c"]);
        expect(keys).toEqual(["0", "1", "2"]);
    });

    test("ignores non-enumerable properties", () => {
        let obj = { foo: 1 };
        Object.defineProperty(obj, "getFoo", {
            value: function () {
                return this.foo;
            },
        });
        keys = Object.keys(obj);
        expect(keys).toEqual(["foo"]);
    });
});

describe("errors", () => {
    test("null argument value", () => {
        expect(() => {
            Object.keys(null);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });

    test("undefined argument value", () => {
        expect(() => {
            Object.keys(undefined);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });
});
