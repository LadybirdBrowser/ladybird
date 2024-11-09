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
        expect(new Function("foo", "if (foo) { return 42; } else { return 'bar'; }")(true)).toBe(
            42
        );
        expect(new Function("foo", "if (foo) { return 42; } else { return 'bar'; }")(false)).toBe(
            "bar"
        );
        expect(new Function("return typeof Function()")()).toBe("function");
        expect(new Function("x", "return function (y) { return x + y };")(1)(2)).toBe(3);

        expect(new Function("-->")()).toBeUndefined();

        expect(new Function().name).toBe("anonymous");
        expect(new Function().toString()).toBe("function anonymous(\n) {\n\n}");
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
            // > eval("\n[")   // Uncaught exception: [SyntaxError] Unexpected token Eof. Expected BracketClose (line: 2, column: 2)
            // > eval("\n[\n") // Uncaught exception: [SyntaxError] Unexpected token Eof. Expected BracketClose (line: 2, column: 3)
            .toThrowWithMessage(
                SyntaxError,
                "Unexpected token Eof. Expected BracketClose (line: 2, column: 3)"
            );
    });

    test("parameters and body must be valid standalone", () => {
        expect(() => {
            new Function("/*", "*/ ) {");
        }).toThrowWithMessage(SyntaxError, "Unterminated multi-line comment (line: 1, column: 3)");
    });
});
