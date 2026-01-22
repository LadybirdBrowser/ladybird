describe("errors", () => {
    test("invalid string", () => {
        expect(() => {
            RegExp.escape(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Symbol(Symbol.hasInstance) is not a string");
    });
});

describe("normal behavior", () => {
    test("first character is alphanumeric", () => {
        const alphanumeric = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

        for (const ch of alphanumeric) {
            const string = `${ch}${ch}${ch}`;
            const expected = `\\x${ch.codePointAt(0).toString(16)}${ch}${ch}`;

            expect(RegExp.escape(string)).toBe(expected);
        }
    });

    test("syntax characters", () => {
        const syntaxCharacters = "^$\\.*+?()[]{}|/";

        for (const ch of syntaxCharacters) {
            const string = `_${ch}_`;
            const expected = `_\\${ch}_`;

            expect(RegExp.escape(string)).toBe(expected);
        }
    });

    test("control characters", () => {
        expect(RegExp.escape("_\t_")).toBe("_\\t_");
        expect(RegExp.escape("_\n_")).toBe("_\\n_");
        expect(RegExp.escape("_\v_")).toBe("_\\v_");
        expect(RegExp.escape("_\f_")).toBe("_\\f_");
        expect(RegExp.escape("_\r_")).toBe("_\\r_");
    });

    test("punctuators", () => {
        const punctuators = ",-=<>#&!%:;@~'`\"";

        for (const ch of punctuators) {
            const string = `_${ch}_`;
            const expected = `_\\x${ch.codePointAt(0).toString(16)}_`;

            expect(RegExp.escape(string)).toBe(expected);
        }
    });

    test("non-ASCII whitespace", () => {
        const nbsp = "\u00A0";

        expect(RegExp.escape("\u00A0")).toBe("\\xa0");
        expect(RegExp.escape("\uFEFF")).toBe("\\ufeff");
        expect(RegExp.escape("\u2028")).toBe("\\u2028");
        expect(RegExp.escape("\u2029")).toBe("\\u2029");
    });

    test("Unicode surrogates", () => {
        for (let ch = 0xd800; ch <= 0xdfff; ++ch) {
            const string = String.fromCodePoint(ch);
            const expected = `\\u${ch.toString(16)}`;

            expect(RegExp.escape(string)).toBe(expected);
        }
    });
});
