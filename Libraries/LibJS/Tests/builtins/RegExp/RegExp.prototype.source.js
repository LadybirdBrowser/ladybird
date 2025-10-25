test("basic functionality", () => {
    expect(RegExp.prototype.source).toBe("(?:)");
    expect(RegExp().source).toBe("(?:)");
    expect(/test/.source).toBe("test");
    expect(/\n/.source).toBe("\\n");
    expect(/foo\/bar/.source).toBe("foo\\/bar");
});

test("escaped characters", () => {
    const tests = [
        { regex: /\\n/, source: "\\\\n" },
        { regex: /\\\n/, source: "\\\\\\n" },
        { regex: /\\\\n/, source: "\\\\\\\\n" },
        { regex: /\\r/, source: "\\\\r" },
        { regex: /\\\r/, source: "\\\\\\r" },
        { regex: /\\\\r/, source: "\\\\\\\\r" },
        { regex: /\\u2028/, source: "\\\\u2028" },
        { regex: /\\\u2028/, source: "\\\\\\u2028" },
        { regex: /\\\\u2028/, source: "\\\\\\\\u2028" },
        { regex: /\\u2029/, source: "\\\\u2029" },
        { regex: /\\\u2029/, source: "\\\\\\u2029" },
        { regex: /\\\\u2029/, source: "\\\\\\\\u2029" },
        { regex: /\//, source: "\\/" },
        { regex: /\\\//, source: "\\\\\\/" },
        { regex: /[/]/, source: "[/]" },
        { regex: /[\/]/, source: "[\\/]" },
        { regex: /[\\/]/, source: "[\\\\/]" },
        { regex: /[\\\/]/, source: "[\\\\\\/]" },
        { regex: /\[\/\]/, source: "\\[\\/\\]" },
        { regex: /\[\/\]/, source: "\\[\\/\\]" },
        { regex: /\[\\\/\]/, source: "\\[\\\\\\/\\]" },
    ];

    for (const test of tests) {
        expect(test.regex.source).toBe(test.source);
    }
});
