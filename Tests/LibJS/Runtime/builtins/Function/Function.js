describe("correct behavior", () => {
    test("constructor properties", () => {
        expect(Function).toHaveLength(1);
        expect(Function.name).toBe("Function");
        expect(Function.prototype).toHaveLength(0);
        expect(Function.prototype.name).toBe("");
    });

    test("typeof", () => {
        expect(typeof Function()).toBe("function");
        expect(typeof new Function()).toBe("function");
    });

    test("basic functionality", () => {
        expect(Function()()).toBeUndefined();
        expect(new Function()()).toBeUndefined();
        expect(Function("return 42")()).toBe(42);
        expect(new Function("return 42")()).toBe(42);
        expect(new Function("foo", "return foo")(42)).toBe(42);
        expect(new Function("foo,bar", "return foo + bar")(1, 2)).toBe(3);
        expect(new Function("foo", "bar", "return foo + bar")(1, 2)).toBe(3);
        expect(new Function("foo", "bar,baz", "return foo + bar + baz")(1, 2, 3)).toBe(6);
        expect(new Function("foo", "bar", "baz", "return foo + bar + baz")(1, 2, 3)).toBe(6);
        expect(new Function("foo", "if (foo) { return 42; } else { return 'bar'; }")(true)).toBe(42);
        expect(new Function("foo", "if (foo) { return 42; } else { return 'bar'; }")(false)).toBe("bar");
        expect(new Function("return typeof Function()")()).toBe("function");
        expect(new Function("x", "return function (y) { return x + y };")(1)(2)).toBe(3);

        expect(new Function("-->")()).toBeUndefined();

        expect(new Function().name).toBe("anonymous");
        expect(new Function().toString()).toBe("function anonymous(\n) {\n\n}");
    });

    test("subclassed constructor returns a function with the subclass prototype", () => {
        class Subclass extends Function {}

        const sub = new Subclass("return 42");
        expect(sub()).toBe(42);
        expect(Object.getPrototypeOf(sub)).toBe(Subclass.prototype);
        expect(sub).toBeInstanceOf(Subclass);
        expect(sub).toBeInstanceOf(Function);
    });
});

describe("errors", () => {
    test("syntax error", () => {
        expect(() => {
            new Function("[");
        })
            // This might be confusing at first but keep in mind it's actually parsing
            // function anonymous() { [ }
            // Since the body, surrounded by a newline on each side, is first parsed standalone,
            // we report unexpected token EOF instead of }.
            // FIXME: The position is odd though, I'd expect `line: 2, column: 2` and `line: 3, column: 1`...
            // > eval("\n[")   // Uncaught exception: [SyntaxError] Unexpected end of input. Expected ']' (line: 2, column: 2)
            // > eval("\n[\n") // Uncaught exception: [SyntaxError] Unexpected end of input. Expected ']' (line: 2, column: 3)
            .toThrowWithMessage(SyntaxError, "Unexpected end of input. Expected ']' (line: 2, column: 3)");
    });

    test("parameters and body must be valid standalone", () => {
        expect(() => {
            new Function("/*", "*/ ) {");
        }).toThrowWithMessage(SyntaxError, "Unterminated multi-line comment (line: 1, column: 3)");
    });

    test("parameters must not end the parameter list", () => {
        const GeneratorFunction = function* () {}.constructor;
        const AsyncFunction = async function () {}.constructor;
        const AsyncGeneratorFunction = async function* () {}.constructor;
        for (const constructor of [Function, GeneratorFunction, AsyncFunction, AsyncGeneratorFunction]) {
            expect(() => constructor("a) { return 1; } function g(b", "return 2;")).toThrow(SyntaxError);
            expect(() => constructor(") {} function g(", "return 2;")).toThrow(SyntaxError);
            expect(() => constructor("a) {}; (0", "")).toThrow(SyntaxError);
            expect(() => constructor("a b", "")).toThrowWithMessage(SyntaxError, "Expected ',' or end of input");
            expect(() => constructor("a, a = 1", "return @")).toThrowWithMessage(
                SyntaxError,
                "Duplicate parameter names not allowed"
            );
            expect(() => constructor("a = function (b) {}, c", "")).not.toThrow();
            expect(() => constructor("a, b // comment", "")).not.toThrow();
            expect(() => constructor("{ a }, [b] = [], ...c", "")).not.toThrow();
        }
    });
});
