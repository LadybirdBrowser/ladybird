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

test("expressions of every kind, many times over", () => {
    const object = { toString: () => "an object" };
    const values = [
        0,
        7,
        999,
        1000,
        100000,
        -1,
        -2147483648,
        2147483647,
        1.5,
        -0,
        NaN,
        "",
        "x",
        "a longer string than most",
    ];
    values.push(true, null, undefined, 10n, object, [1, 2]);
    const describe = value => `value ${value} of ${values.length}`;
    for (let i = 0; i < 100; ++i) {
        for (const value of values) {
            const expected = "value " + String(value) + " of 20";
            expect(describe(value)).toBe(expected);
            expect(describe(value).length).toBe(expected.length);
        }
        expect(`${""}${""}`).toBe("");
        expect(() => describe(Symbol())).toThrow(TypeError);
    }
});

test("toString() of a substitution changing what the template literal read before", () => {
    const object = { x: 1, value: 5 };
    const bump = {
        toString() {
            object.x++;
            return "bumped";
        },
    };
    const read = (object, prefix) => {
        const text = `${prefix}${object.x}${object.value}`;
        return object.x;
    };
    for (let i = 0; i < 1000; ++i) {
        object.value = i % 2 ? bump : i;
        const expected = object.x + (i % 2);
        expect(read(object, "prefix")).toBe(expected);
    }
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

test("empty template literal", () => {
    expect(``).toBe("");
});

test("const fold template literal concatenation", () => {
    expect(`abc` + "xyz").toBe("abcxyz");
    expect(`abc` + `xyz`).toBe("abcxyz");
    expect("abc" + `xyz`).toBe("abcxyz");
    expect("abc" + "xyz").toBe("abcxyz");
    expect(`abc` + `` + `xyz`).toBe("abcxyz");
    expect(`abc` + "" + `xyz`).toBe("abcxyz");
    expect(`${"abcxyz"}`).toBe("abcxyz");
    // TODO: not const fold-able (yet)
    expect(`${"abc"}${"xyz"}`).toBe("abcxyz");
});
