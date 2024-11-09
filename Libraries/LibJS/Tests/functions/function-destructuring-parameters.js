describe("parsing", () => {
    test("single parameter, single name", () => {
        expect(`function testFunction({ a }) { }`).toEval();
    });

    test("single parameter, single name with rest values", () => {
        expect(`function testFunction({ a, ...b }) { }`).toEval();
    });

    test("single parameter, single aliased name", () => {
        expect(`function testFunction({ a: b }) { }`).toEval();
    });

    test("single parameter, single aliased name with rest values", () => {
        expect(`function testFunction({ a: b, ...c }) { }`).toEval();
    });

    test("multiple parameters, single destructuring parameter", () => {
        expect(`function testFunction(a, { b }) { }`).toEval();
    });

    test("multiple destructuring parameters", () => {
        expect(`function testFunction({ a }, { b }) { }`).toEval();
    });

    test("multiple destructuring parameters with rest parameters", () => {
        expect(`function testFunction({ a }, { bar, ...b }) { }`).toEval();
    });

    test("multiple destructuring parameters with rest parameters 2", () => {
        expect(`function testFunction({ bar, ...a }, { bar, ...b }) { }`).toEval();
    });

    test("multiple destructuring parameters, array patterns", () => {
        expect(`function testFunction({ bar, ...a }, [ b ]) { }`).toEval();
    });

    test("multiple destructuring parameters with rest parameters, array patterns", () => {
        expect(`function testFunction({ bar, ...a }, [ b, ...rest ]) { }`).toEval();
    });

    test("multiple destructuring parameters with rest parameters, array patterns with recursive patterns", () => {
        expect(`function testFunction({ bar, ...a }, [ b, [ c ] ]) { }`).toEval();
    });

    test("multiple destructuring parameters with rest parameters, array patterns with recursive patterns 2", () => {
        expect(`function testFunction({ bar, ...a }, [ b, [ c, ...{ d } ] ]) { }`).toEval();
    });
});

describe("evaluating", () => {
    test("single parameter, single name", () => {
        function testFunction({ a }) {
            return a;
        }

        expect(testFunction({ a: 42 })).toBe(42);
    });

    test("single parameter, single name with rest values", () => {
        function testFunction({ a, ...b }) {
            return b.foo;
        }

        expect(testFunction({ a: 42, foo: "yoinks" })).toBe("yoinks");
    });

    test("single parameter, single aliased name", () => {
        function testFunction({ a: b }) {
            return b;
        }

        expect(testFunction({ a: 42, foo: "yoinks" })).toBe(42);
    });

    test("single parameter, single aliased name with rest values", () => {
        function testFunction({ a: b, ...c }) {
            return b + c.foo;
        }

        expect(testFunction({ a: 42, foo: "yoinks" })).toBe("42yoinks");
    });

    test("multiple parameters, single destructuring parameter", () => {
        function testFunction(a, { b }) {
            return a + b;
        }

        expect(testFunction("27", { b: 42 })).toBe("2742");
    });

    test("multiple destructuring parameters", () => {
        function testFunction({ a }, { b }) {
            return a + b;
        }

        expect(testFunction({ a: "27" }, { b: 42 })).toBe("2742");
    });

    test("multiple destructuring parameters with rest parameters", () => {
        function testFunction({ a }, { bar, ...b }) {
            return bar;
        }

        expect(testFunction({ a: "27" }, { b: 42 })).toBeUndefined();
    });

    test("multiple destructuring parameters with rest parameters 2", () => {
        function testFunction({ bar, ...a }, { bar, ...b }) {
            return a.foo + b.foo;
        }

        expect(testFunction({ foo: "27" }, { foo: 42 })).toBe("2742");
    });

    test("multiple destructuring parameters, array patterns", () => {
        function testFunction({ bar, ...a }, [b]) {
            return a.foo + b;
        }

        expect(testFunction({ foo: "27" }, [42])).toBe("2742");
    });

    test("multiple destructuring parameters with rest parameters, array patterns", () => {
        function testFunction({ bar, ...a }, [b, ...rest]) {
            return rest.length + a.foo;
        }

        expect(testFunction({ foo: "20" }, [0, 1, 2, 3, 4])).toBe("420");
    });

    test("multiple destructuring parameters with rest parameters, array patterns with recursive patterns", () => {
        function testFunction({ bar, ...a }, [b, [c]]) {
            return a.foo + b + c;
        }

        expect(testFunction({ foo: "20" }, [0, [1, 2, 3, 4]])).toBe("2001");
    });

    test("multiple destructuring parameters with rest parameters, array patterns with recursive patterns 2", () => {
        function testFunction({ bar, ...a }, [b, [c, ...{ length }]]) {
            return a.foo + b + c + length;
        }

        expect(testFunction({ foo: "20" }, [0, [1, 2, 3, 4]])).toBe("20013");
    });

    test("patterns with default", () => {
        function testFunction({ a = "27", b: bar }) {
            return a + bar;
        }

        expect(testFunction({ b: 42 })).toBe("2742");
    });
});
