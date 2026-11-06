describe("syntax error messages", () => {
    function expectMessage(source, message) {
        expect(source).not.toEval();
        expect(() => eval(source)).toThrowWithMessage(SyntaxError, message);
    }

    test("name the unexpected token as the source spells it", () => {
        expectMessage("let x = ;", "Unexpected token ';'. Expected primary expression");
        expectMessage("x y", "Unexpected token 'y'. Expected ';'");
        expectMessage("if (1) )", "Unexpected token ')'. Expected statement");
    });

    test("name the end of the input", () => {
        expectMessage("if (x) { y", "Unexpected end of input. Expected '}'");
        expectMessage("(", "Unexpected end of input");
    });

    test("name both ways a list can go on", () => {
        expectMessage("foo(1, 2", "Unexpected end of input. Expected ',' or ')'");
        expectMessage("function f(a b) {}", "Unexpected token 'b'. Expected ',' or ')'");
        expectMessage("[1 2]", "Unexpected token '2'. Expected ',' or ']'");
        expectMessage("({ a: 1 b: 2 })", "Unexpected token 'b'. Expected ',' or '}'");
    });

    test("cut long tokens short", () => {
        const name = "a".repeat(100);
        expectMessage(`x ${name}`, `Unexpected token '${"a".repeat(40)}...'. Expected ';'`);
    });

    test("unterminated literals", () => {
        expectMessage('"abc', "Unterminated string literal");
        expectMessage("`abc", "Unterminated template literal");
        expectMessage("/abc", "Unterminated regular expression literal");
    });
});
