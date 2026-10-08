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

test("keys stay right as objects of the same shape change", () => {
    const make = () => ({ b: 1, a: 2, [Symbol("s")]: 3 });
    const first = make();
    expect(Object.keys(first)).toEqual(["b", "a"]);
    const second = make();
    expect(Object.keys(second)).toEqual(["b", "a"]);
    Object.defineProperty(second, "hidden", { value: 1, enumerable: false });
    second.c = 3;
    expect(Object.keys(second)).toEqual(["b", "a", "c"]);
    delete second.b;
    expect(Object.keys(second)).toEqual(["a", "c"]);
    second.b = 4;
    expect(Object.keys(second)).toEqual(["a", "c", "b"]);
    Object.defineProperty(second, "a", { enumerable: false });
    expect(Object.keys(second)).toEqual(["c", "b"]);
    expect(Object.keys(first)).toEqual(["b", "a"]);

    const withIndices = make();
    withIndices[1] = "x";
    withIndices[0] = "y";
    expect(Object.keys(withIndices)).toEqual(["0", "1", "b", "a"]);

    const dictionary = {};
    for (let i = 0; i < 100; ++i) dictionary["k" + i] = i;
    for (let i = 0; i < 98; ++i) delete dictionary["k" + i];
    expect(Object.keys(dictionary)).toEqual(["k98", "k99"]);
    dictionary.z = 1;
    expect(Object.keys(dictionary)).toEqual(["k98", "k99", "z"]);
});
