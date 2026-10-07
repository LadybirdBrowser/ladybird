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

describe("objects sharing a shape", () => {
    test("keys follow property changes of objects with the same shape", () => {
        const make = () => ({ a: 1, b: 2 });
        const first = make();
        const second = make();
        expect(Object.keys(first)).toEqual(["a", "b"]);
        second.c = 3;
        expect(Object.keys(second)).toEqual(["a", "b", "c"]);
        expect(Object.keys(first)).toEqual(["a", "b"]);
        delete second.a;
        expect(Object.keys(second)).toEqual(["b", "c"]);
        Object.defineProperty(second, "b", { enumerable: false });
        expect(Object.keys(second)).toEqual(["c"]);
        expect(Object.getOwnPropertyNames(second)).toEqual(["b", "c"]);
        expect(Object.keys(make())).toEqual(["a", "b"]);
    });

    test("returned arrays are fresh", () => {
        const object = { a: 1 };
        const keys = Object.keys(object);
        keys.push("x");
        expect(Object.keys(object)).toEqual(["a"]);
    });

    test("dictionary objects", () => {
        const object = {};
        const expected = [];
        for (let i = 0; i < 100; ++i) {
            object["p" + i] = i;
            expected.push("p" + i);
        }
        expect(Object.keys(object)).toEqual(expected);
        delete object.p50;
        expected.splice(50, 1);
        expect(Object.keys(object)).toEqual(expected);
        Object.defineProperty(object, "p60", { enumerable: false });
        expect(Object.keys(object)).not.toContain("p60");
        expect(Object.getOwnPropertyNames(object)).toContain("p60");
        object.p50 = 50;
        expect(Object.keys(object).at(-1)).toBe("p50");
    });

    test("symbols, functions and accessors", () => {
        const symbol = Symbol("s");
        const object = { a: 1, [symbol]: 2, get b() {} };
        expect(Object.keys(object)).toEqual(["a", "b"]);
        expect(Object.getOwnPropertyNames(object)).toEqual(["a", "b"]);
        function f() {}
        f.x = 1;
        expect(Object.keys(f)).toEqual(["x"]);
        expect(Object.getOwnPropertyNames(f)).toContain("prototype");
        const args = (function (a) {
            return arguments;
        })(1);
        expect(Object.getOwnPropertyNames(args)).toEqual(["0", "length", "callee"]);
    });

    test("indexed properties come first", () => {
        const object = { a: 1 };
        object[1] = 2;
        object[0] = 3;
        expect(Object.keys(object)).toEqual(["0", "1", "a"]);
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
