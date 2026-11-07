describe("escapes in a v-mode class", () => {
    test("every ClassSetReservedPunctuator can be escaped", () => {
        for (const punctuator of "&-!#%,:;<=>@`~") {
            const regex = new RegExp(`[\\${punctuator}]`, "v");
            expect(regex.test(punctuator)).toBeTrue();
            expect(regex.test("a")).toBeFalse();
        }
    });

    test("an escaped punctuator works in ranges and set operations", () => {
        expect(/[\&-\)]/v.test("'")).toBeTrue();
        expect(/[\p{L}--\&]/v.test("a")).toBeTrue();
        expect(/[\p{L}--\&]/v.test("&")).toBeFalse();
        expect(/[\&&&\&]/v.test("&")).toBeTrue();
        expect(/[\!\!]/v.test("!")).toBeTrue();
    });

    test("a control escape", () => {
        expect(/[\cJ]/v.test("\n")).toBeTrue();
        expect(/[\q{\cJ}]/v.test("\n")).toBeTrue();
    });

    test("\\q{} takes the same escapes", () => {
        expect(/[\q{\&}]/v.test("&")).toBeTrue();
        expect(/^[\q{a\!b}]$/v.test("a!b")).toBeTrue();
        expect(/[\q{\/}]/v.test("/")).toBeTrue();
        expect(/[\q{\^}]/v.test("^")).toBeTrue();
    });

    test("other escapes are still syntax errors", () => {
        const cases = [
            ["[\\_]", "invalid escape '\\_'"],
            ['[\\"]', `invalid escape '\\"'`],
            ["[\\']", "invalid escape '\\''"],
            ["[\\c1]", "invalid escape '\\c'"],
            ["[\\q{\\_}]", "invalid escape '\\_'"],
            ["[\\q{\\01}]", "invalid escape '\\0'"],
        ];
        for (const [pattern, message] of cases) {
            expect(() => new RegExp(pattern, "v")).toThrowWithMessage(SyntaxError, message);
        }
    });
});

test("a reserved punctuator still can't be escaped outside a class", () => {
    expect(() => new RegExp("\\&", "v")).toThrowWithMessage(SyntaxError, "invalid escape '\\&'");
});

test("a reserved double punctuator still can't appear unescaped in a class", () => {
    expect(() => new RegExp("[!!]", "v")).toThrowWithMessage(SyntaxError, "invalid character class");
});
