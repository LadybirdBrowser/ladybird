describe("correct behavior", () => {
    test("lengths", () => {
        expect(Object.values).toHaveLength(1);
        expect(Object.values(true)).toHaveLength(0);
        expect(Object.values(45)).toHaveLength(0);
        expect(Object.values(-998)).toHaveLength(0);
        expect(Object.values("abcd")).toHaveLength(4);
        expect(Object.values([1, 2, 3])).toHaveLength(3);
        expect(Object.values({ a: 1, b: 2, c: 3 })).toHaveLength(3);
    });

    test("object argument", () => {
        let values = Object.values({ foo: 1, bar: 2, baz: 3 });
        expect(values).toEqual([1, 2, 3]);
    });

    test("object argument with symbol keys", () => {
        let values = Object.values({ foo: 1, [Symbol("bar")]: 2, baz: 3 });
        expect(values).toEqual([1, 3]);
    });

    test("array argument", () => {
        let values = Object.values(["a", "b", "c"]);
        expect(values).toEqual(["a", "b", "c"]);
    });

    test("ignores non-enumerable properties", () => {
        let obj = { foo: 1 };
        Object.defineProperty(obj, "getFoo", {
            value: function () {
                return this.foo;
            },
        });
        let values = Object.values(obj);
        expect(values).toEqual([1]);
    });
});

describe("errors", () => {
    test("null argument", () => {
        expect(() => {
            Object.values(null);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });

    test("undefined argument", () => {
        expect(() => {
            Object.values(undefined);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });
});
