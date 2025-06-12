/*
These tests deliberately produce syntax errors to check what line the parser thinks we're on.
Note that line numbers are higher than you might expect as the parsed code is:

function anonymous(
) {
<code>
}

⚠ PLEASE MAKE SURE TO NOT LET YOUR EDITOR REMOVE THE LS/PS LINE TERMINATORS!
*/

test("LINE FEED is a line terminator", () => {
    expect(() => {
        Function("\n\n@");
    }).toThrowWithMessage(
        SyntaxError,
        "Unexpected token Invalid. Expected CurlyClose (line: 4, column: 1)"
    );
});

test("CARRIAGE RETURN is a line terminator", () => {
    expect(() => {
        Function("\r\r@");
    }).toThrowWithMessage(
        SyntaxError,
        "Unexpected token Invalid. Expected CurlyClose (line: 4, column: 1)"
    );
});

test("LINE SEPARATOR is a line terminator", () => {
    expect(() => {
        Function("  @");
    }).toThrowWithMessage(
        SyntaxError,
        "Unexpected token Invalid. Expected CurlyClose (line: 4, column: 1)"
    );
});

test("PARAGRAPH SEPARATOR is a line terminator", () => {
    expect(() => {
        Function("  @");
    }).toThrowWithMessage(
        SyntaxError,
        "Unexpected token Invalid. Expected CurlyClose (line: 4, column: 1)"
    );
});

test("CR LF is counted as only one line terminator", () => {
    expect(() => {
        Function("\r\n\r\n@");
    }).toThrowWithMessage(
        SyntaxError,
        "Unexpected token Invalid. Expected CurlyClose (line: 4, column: 1)"
    );
});

test("LF/CR are not allowed in string literal", () => {
    expect(() => {
        Function(`"
        "`);
    }).toThrowWithMessage(SyntaxError, "Unexpected token UnterminatedStringLiteral");
});

test("LS/PS are allowed in string literal", () => {
    expect(`" "`).toEval();
    expect(`" "`).toEval();
});

test("line terminators can be mixed (but please don't)", () => {
    expect(() => {
        Function("\r \r\n \n\r@");
    }).toThrowWithMessage(
        SyntaxError,
        "Unexpected token Invalid. Expected CurlyClose (line: 8, column: 1)"
    );
});

test("all line terminators are valid for line continuations", () => {
    expect(Function('return "a\\\nb"')()).toBe("ab");
    expect(Function('return "a\\\rb"')()).toBe("ab");
    expect(Function('return "a\\\r\nb"')()).toBe("ab");
    expect(Function('return "a\\ b"')()).toBe("ab");
    expect(Function('return "a\\ b"')()).toBe("ab");
});

test("template-literals raw and real value", () => {
    let lastTemplate;
    let lastRaw;

    function tag(cs) {
        lastTemplate = cs[0];
        lastRaw = cs.raw[0];
    }

    function checkTemplate(string_value, expected_template, expected_raw) {
        eval("tag`" + string_value + "`");
        expect(lastTemplate).toBe(expected_template);
        expect(lastRaw).toBe(expected_raw);
    }

    checkTemplate("", "", "");
    checkTemplate("\n", "\n", "\n");
    checkTemplate("\r", "\n", "\n");
    checkTemplate("\r\n", "\n", "\n");
    checkTemplate("\n\r\n", "\n\n", "\n\n");

    checkTemplate("a\\\nb", "ab", "a\\\nb");
    checkTemplate("a\\\rb", "ab", "a\\\nb");
    checkTemplate("a\\ b", "ab", "a\\ b");
    checkTemplate("a\\ b", "ab", "a\\ b");
});
