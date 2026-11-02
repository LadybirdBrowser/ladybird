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

    test("result arrays ignore inherited indexed setters", () => {
        Object.defineProperty(Array.prototype, "0", {
            configurable: true,
            set() {
                throw new Error("Inherited setter must not be called");
            },
        });

        let entries;
        try {
            entries = Object.entries({ foo: 1 });
        } finally {
            delete Array.prototype[0];
        }

        expect(entries).toEqual([["foo", 1]]);
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

    test("delete key from getter", () => {
        const obj = {
            get a() {
                delete this.b;
                return 1;
            },
            b: 2,
        };

        expect(Object.entries(obj)).toEqual([["a", 1]]);
    });

    test("indexed property changed by a getter", () => {
        const deleted = {};
        Object.defineProperty(deleted, 0, {
            get() {
                delete deleted[1];
                return "a";
            },
            enumerable: true,
            configurable: true,
        });
        deleted[1] = "b";
        expect(Object.entries(deleted)).toEqual([["0", "a"]]);

        const hidden = {};
        Object.defineProperty(hidden, 0, {
            get() {
                Object.defineProperty(hidden, 1, { enumerable: false });
                return "a";
            },
            enumerable: true,
            configurable: true,
        });
        hidden[1] = "b";
        expect(Object.entries(hidden)).toEqual([["0", "a"]]);
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
