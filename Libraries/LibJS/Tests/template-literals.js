test("plain literals with expression-like characters", () => {
    expect(`foo`).toBe("foo");
    expect(`foo{`).toBe("foo{");
    expect(`foo}`).toBe("foo}");
    expect(`foo$`).toBe("foo$");
});

test("plain literals with escaped special characters", () => {
    expect(`foo\``).toBe("foo`");
    expect(`foo\\`).toBe("foo\\");
    expect(`foo\\\``).toBe("foo\\`");
    expect(`foo\\\\`).toBe("foo\\\\");
    expect(`foo\$`).toBe("foo$");
    expect(`foo \${"bar"}`).toBe('foo ${"bar"}');
});

test("literals in expressions", () => {
    expect(`foo ${undefined}`).toBe("foo undefined");
    expect(`foo ${null}`).toBe("foo null");
    expect(`foo ${5}`).toBe("foo 5");
    expect(`foo ${true}`).toBe("foo true");
    expect(`foo ${"bar"}`).toBe("foo bar");
});

test("objects in expressions", () => {
    expect(`foo ${{}}`).toBe("foo [object Object]");
    expect(`foo ${{ bar: { baz: "qux" } }}`).toBe("foo [object Object]");
});

test("expressions at beginning of template literal", () => {
    expect(`${"foo"} bar baz`).toBe("foo bar baz");
    expect(`${"foo bar baz"}`).toBe("foo bar baz");
});

test("multiple template literals", () => {
    expect(`foo ${"bar"} ${"baz"}`).toBe("foo bar baz");
});

test("variables in expressions", () => {
    let a = 27;
    expect(`${a}`).toBe("27");
    expect(`foo ${a}`).toBe("foo 27");
    expect(`foo ${a ? "bar" : "baz"}`).toBe("foo bar");
    expect(`foo ${(() => a)()}`).toBe("foo 27");
});

test("template literals in expressions", () => {
    expect(`foo ${`bar`}`).toBe("foo bar");
    expect(`${`${`${`${"foo"}`} bar`}`}`).toBe("foo bar");
});

test("newline literals (not characters)", () => {
    expect(
        `foo
    bar`
    ).toBe("foo\n    bar");
});

test("line continuation in literals (not characters)", () => {
    expect(
        `foo\
    bar`
    ).toBe("foo    bar");
});

test("reference error from expressions", () => {
    expect(() => `${b}`).toThrowWithMessage(ReferenceError, "'b' is not defined");
});

test("invalid escapes should give syntax error", () => {
    expect("`\\u`").not.toEval();
    expect("`\\01`").not.toEval();
    expect("`\\u{10FFFFF}`").not.toEval();
});
