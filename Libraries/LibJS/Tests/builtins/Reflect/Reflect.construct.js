test("length is 2", () => {
    expect(Reflect.construct).toHaveLength(2);
});

describe("errors", () => {
    test("target must be a constructor", () => {
        [null, undefined, "foo", 123, NaN, Infinity, {}].forEach(value => {
            expect(() => {
                Reflect.construct(value);
            }).toThrowWithMessage(TypeError, `${value} is not a constructor`);
        });
    });

    test("arguments list must be an object", () => {
        [null, undefined, "foo", 123, NaN, Infinity].forEach(value => {
            expect(() => {
                Reflect.construct(function () {}, value);
            }).toThrowWithMessage(TypeError, `${value} is not an object`);
        });
    });

    test("new target must be a constructor", () => {
        [null, undefined, "foo", 123, NaN, Infinity, {}].forEach(value => {
            expect(() => {
                Reflect.construct(function () {}, [], value);
            }).toThrowWithMessage(TypeError, `${value} is not a constructor`);
        });
    });
});

describe("normal behavior", () => {
    test("built-in Array function", () => {
        var a = Reflect.construct(Array, [5]);
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(5);
    });

    test("built-in String function", () => {
        var s = Reflect.construct(String, [123]);
        expect(s instanceof String).toBeTrue();
        expect(s).toHaveLength(3);
        expect(s.toString()).toBe("123");
    });

    test("user-defined function", () => {
        function Foo() {
            this.name = "foo";
        }

        var o = Reflect.construct(Foo, []);
        expect(o.name).toBe("foo");
        expect(o instanceof Foo).toBeTrue();
    });

    test("user-defined function with different new target", () => {
        function Foo() {
            this.name = "foo";
        }

        function Bar() {
            this.name = "bar";
        }

        var o = Reflect.construct(Foo, [], Bar);
        expect(o.name).toBe("foo");
        expect(o instanceof Foo).toBeFalse();
        expect(o instanceof Bar).toBeTrue();
    });
});
