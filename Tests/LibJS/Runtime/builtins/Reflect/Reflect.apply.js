test("length is 3", () => {
    expect(Reflect.apply).toHaveLength(3);
});

describe("errors", () => {
    test("target must be a function", () => {
        [null, undefined, "foo", 123, NaN, Infinity, {}].forEach(value => {
            expect(() => {
                Reflect.apply(value);
            }).toThrowWithMessage(TypeError, `${value} is not a function`);
        });
    });

    test("arguments list must be an object", () => {
        [null, undefined, "foo", 123, NaN, Infinity].forEach(value => {
            expect(() => {
                Reflect.apply(() => {}, undefined, value);
            }).toThrowWithMessage(TypeError, `${value} is not an object`);
        });
    });

    test("an array-like longer than any call can take throws before its elements are read", () => {
        [Infinity, 1e15, 2 ** 53 - 1, 2 ** 28].forEach(length => {
            const arrayLike = {
                length,
                get 0() {
                    throw new Error("element read");
                },
            };
            expect(() => {
                Reflect.apply(() => {}, undefined, arrayLike);
            }).toThrowWithMessage(InternalError, "Call stack size limit exceeded");
        });
    });
});

describe("normal behavior", () => {
    test("calling built-in functions", () => {
        expect(Reflect.apply(String.prototype.charAt, "foo", [0])).toBe("f");
        expect(Reflect.apply(Array.prototype.indexOf, ["hello", 123, "foo", "bar"], ["foo"])).toBe(2);
    });

    test("|this| argument is forwarded to called function", () => {
        function Foo(foo) {
            this.foo = foo;
        }

        var o = {};
        expect(o.foo).toBeUndefined();
        Reflect.apply(Foo, o, ["bar"]);
        expect(o.foo).toBe("bar");
    });
});
