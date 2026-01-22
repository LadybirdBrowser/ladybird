describe("correct behavior", () => {
    test("length", () => {
        expect(JSON.stringify).toHaveLength(3);
    });

    test("basic functionality", () => {
        [
            [5, "5"],
            [undefined, undefined],
            [null, "null"],
            [NaN, "null"],
            [-NaN, "null"],
            [Infinity, "null"],
            [-Infinity, "null"],
            [true, "true"],
            [false, "false"],
            ["test", '"test"'],
            [new Number(5), "5"],
            [new Boolean(false), "false"],
            [new String("test"), '"test"'],
            [() => {}, undefined],
            [[1, 2, "foo"], '[1,2,"foo"]'],
            [{ foo: 1, bar: "baz", qux() {} }, '{"foo":1,"bar":"baz"}'],
            [
                {
                    var1: 1,
                    var2: 2,
                    toJSON(key) {
                        let o = this;
                        o.var2 = 10;
                        return o;
                    },
                },
                '{"var1":1,"var2":10}',
            ],
        ].forEach(testCase => {
            expect(JSON.stringify(testCase[0])).toEqual(testCase[1]);
        });
    });

    test("serialize BigInt with a toJSON property", () => {
        Object.defineProperty(BigInt.prototype, "toJSON", {
            configurable: true, // Allows deleting this property at the end of this test case.
            get() {
                "use strict";
                return () => typeof this;
            },
        });

        expect(JSON.stringify(1n)).toBe('"bigint"');
        delete BigInt.prototype.toJSON;
    });

    test("ignores non-enumerable properties", () => {
        let o = { foo: "bar" };
        Object.defineProperty(o, "baz", { value: "qux", enumerable: false });
        expect(JSON.stringify(o)).toBe('{"foo":"bar"}');
    });

    test("ignores symbol properties", () => {
        let o = { foo: "bar" };
        let sym = Symbol("baz");
        o[sym] = "qux";
        expect(JSON.stringify(o)).toBe('{"foo":"bar"}');
    });

    test("escape surrogate codepoints in strings", () => {
        expect(JSON.stringify("\ud83d\ude04")).toBe('"😄"');
        expect(JSON.stringify("\ud83d")).toBe('"\\ud83d"');
        expect(JSON.stringify("\ude04")).toBe('"\\ude04"');
        expect(JSON.stringify("\ud83d\ud83d\ude04\ud83d\ude04\ude04")).toBe('"\\ud83d😄😄\\ude04"');
        expect(JSON.stringify("\ude04\ud83d\ude04\ud83d\ude04\ud83d")).toBe('"\\ude04😄😄\\ud83d"');
    });

    test("special number values", () => {
        expect(JSON.stringify(-0)).toBe("0");
        expect(JSON.stringify(1e20)).toBe("100000000000000000000");
        expect(JSON.stringify(1e21)).toBe("1e+21");
        expect(JSON.stringify(Number.MAX_VALUE)).toBe("1.7976931348623157e+308");
        expect(JSON.stringify(Number.MIN_VALUE)).toBe("5e-324");
    });

    test("string escape sequences", () => {
        expect(JSON.stringify('"')).toBe('"\\""');
        expect(JSON.stringify("\\")).toBe('"\\\\"');
        expect(JSON.stringify("/")).toBe('"/"');
        expect(JSON.stringify("\b")).toBe('"\\b"');
        expect(JSON.stringify("\f")).toBe('"\\f"');
        expect(JSON.stringify("\n")).toBe('"\\n"');
        expect(JSON.stringify("\r")).toBe('"\\r"');
        expect(JSON.stringify("\t")).toBe('"\\t"');
        expect(JSON.stringify("\0")).toBe('"\\u0000"');
        expect(JSON.stringify("\x1f")).toBe('"\\u001f"');
    });

    test("undefined and functions in arrays become null", () => {
        expect(JSON.stringify([undefined])).toBe("[null]");
        expect(JSON.stringify([function () {}])).toBe("[null]");
        expect(JSON.stringify([Symbol()])).toBe("[null]");
        expect(JSON.stringify([1, undefined, 2])).toBe("[1,null,2]");
    });

    test("undefined and functions in objects are omitted", () => {
        expect(JSON.stringify({ a: undefined })).toBe("{}");
        expect(JSON.stringify({ a: function () {} })).toBe("{}");
        expect(JSON.stringify({ a: Symbol() })).toBe("{}");
        expect(JSON.stringify({ a: 1, b: undefined, c: 2 })).toBe('{"a":1,"c":2}');
    });

    test("array replacer acts as property whitelist", () => {
        expect(JSON.stringify({ a: 1, b: 2, c: 3 }, ["a", "c"])).toBe('{"a":1,"c":3}');
        expect(JSON.stringify({ a: { b: 1, c: 2 } }, ["a", "b"])).toBe('{"a":{"b":1}}');
    });

    test("space parameter formats output", () => {
        expect(JSON.stringify({ a: 1 }, null, 2)).toBe('{\n  "a": 1\n}');
        expect(JSON.stringify({ a: 1 }, null, "\t")).toBe('{\n\t"a": 1\n}');
        expect(JSON.stringify([1, 2], null, 2)).toBe("[\n  1,\n  2\n]");
        // Space capped at 10
        expect(JSON.stringify({ a: 1 }, null, 20)).toBe('{\n          "a": 1\n}');
    });

    test("sparse arrays", () => {
        expect(JSON.stringify(Array(3))).toBe("[null,null,null]");
    });
});

describe("errors", () => {
    test("cannot serialize BigInt", () => {
        expect(() => {
            JSON.stringify(5n);
        }).toThrow(TypeError, "Cannot serialize BigInt value to JSON");
    });

    test("cannot serialize circular structures", () => {
        let bad1 = {};
        bad1.foo = bad1;
        let bad2 = [];
        bad2[5] = [[[bad2]]];

        let bad3a = { foo: "bar" };
        let bad3b = [1, 2, bad3a];
        bad3a.bad = bad3b;

        [bad1, bad2, bad3a].forEach(bad => {
            expect(() => {
                JSON.stringify(bad);
            }).toThrow(TypeError, "Cannot stringify circular object");
        });
    });
});
