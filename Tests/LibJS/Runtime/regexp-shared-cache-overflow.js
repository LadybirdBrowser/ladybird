test("compiled RegExps remain valid after many distinct patterns", () => {
    // Exceed the shared compile cache and make sure each RegExp still
    // matches its own pattern. Patterns past the cap are owned by the
    // RegExpObject rather than the shared table.
    const expressions = [];
    for (let i = 0; i < 1500; i++) {
        const re = new RegExp("^a" + i + "$");
        expressions.push({ re, expected: "a" + i });
    }
    for (const { re, expected } of expressions) {
        expect(re.test(expected)).toBe(true);
        expect(re.test(expected + "x")).toBe(false);
    }

    // A RegExp compiled before the cap was reached still matches correctly.
    expect(expressions[0].re.test(expressions[0].expected)).toBe(true);

    // Re-initialising a RegExpObject drops the previously-owned compile.
    const reused = expressions[500].re;
    reused.compile("^zzz$");
    expect(reused.test("zzz")).toBe(true);
    expect(reused.test("a500")).toBe(false);
});
