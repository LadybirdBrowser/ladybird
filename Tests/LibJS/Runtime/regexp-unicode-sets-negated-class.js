test("triple & in unicode sets class is a syntax error", () => {
    expect(() => eval("/[a&&&]/v")).toThrow(SyntaxError);
    expect(() => eval("/[a&&b&&&c]/v")).toThrow(SyntaxError);
});

test("empty \\q{} in negated unicode sets class is a syntax error", () => {
    expect(() => eval("/[^\\q{}]/v")).toThrow(SyntaxError);
});

test("multi-char string intersection where all operands contain strings is a syntax error in negated class", () => {
    expect(() => eval("/[^\\q{foo}&&\\q{bar}]/v")).toThrow(SyntaxError);
    expect(() => eval("/[^\\q{foo}&&\\q{foo}]/v")).toThrow(SyntaxError);
});

test("multi-char string in negated unicode sets class union is a syntax error", () => {
    expect(() => eval("/[^\\q{ab}]/v")).toThrow(SyntaxError);
    expect(() => eval("/[^\\q{foo}]/v")).toThrow(SyntaxError);
});

test("subtraction with string-valued left operand is a syntax error in negated class", () => {
    expect(() => eval("/[^\\q{foo}--[a-z]]/v")).toThrow(SyntaxError);
});

test("valid unicode sets class set operations in negated class", () => {
    expect(() => eval("/[^\\q{a}]/v")).not.toThrow();
    expect(() => eval("/[^\\q{foo}&&[a]]/v")).not.toThrow();
    expect(() => eval("/[^\\q{a}&&\\q{b}]/v")).not.toThrow();
    expect(() => eval("/[^\\q{a}&&[a-z]]/v")).not.toThrow();
    expect(() => eval("/[^[a-z]--\\q{foo}]/v")).not.toThrow();
    expect(() => eval("/[a&&b]/v")).not.toThrow();
    expect(() => eval("/[a&&b&&c]/v")).not.toThrow();
});
