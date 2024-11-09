describe("basic functionality", () => {
    test("length", () => {
        expect(Object.entries).toHaveLength(1);
        expect(Object.entries(true)).toHaveLength(0);
        expect(Object.entries(45)).toHaveLength(0);
        expect(Object.entries(-998)).toHaveLength(0);
        expect(Object.entries("abcd")).toHaveLength(4);
        expect(Object.entries([1, 2, 3])).toHaveLength(3);
        expect(Object.entries({ a: 1, b: 2, c: 3 })).toHaveLength(3);
    });

    test("entries with object", () => {
        let entries = Object.entries({ foo: 1, bar: 2, baz: 3 });

        expect(entries).toEqual([
            ["foo", 1],
            ["bar", 2],
            ["baz", 3],
        ]);
    });

    test("entries with objects with symbol keys", () => {
        let entries = Object.entries({ foo: 1, [Symbol("bar")]: 2, baz: 3 });

        expect(entries).toEqual([
            ["foo", 1],
            ["baz", 3],
        ]);
    });

    test("entries with array", () => {
        entries = Object.entries(["a", "b", "c"]);
        expect(entries).toEqual([
            ["0", "a"],
            ["1", "b"],
            ["2", "c"],
        ]);
    });

    test("ignores non-enumerable properties", () => {
        let obj = { foo: 1 };
        Object.defineProperty(obj, "getFoo", {
            value: function () {
                return this.foo;
            },
        });
        let entries = Object.entries(obj);
        expect(entries).toEqual([["foo", 1]]);
    });
});

describe("errors", () => {
    test("null argument", () => {
        expect(() => {
            Object.entries(null);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });

    test("undefined argument", () => {
        expect(() => {
            Object.entries(undefined);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });
});
