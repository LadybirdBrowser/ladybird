describe("errors", () => {
    test("invalid pattern", () => {
        expect(() => {
            RegExp("[");
        }).toThrowWithMessage(SyntaxError, "RegExp compile error: Error during parsing of regular expression:");
    });

    test("invalid flag", () => {
        expect(() => {
            RegExp("", "x");
        }).toThrowWithMessage(SyntaxError, "Invalid RegExp flag 'x'");
    });

    test("repeated flag", () => {
        expect(() => {
            RegExp("", "gg");
        }).toThrowWithMessage(SyntaxError, "Repeated RegExp flag 'g'");
    });
});

test("basic functionality", () => {
    expect(RegExp().toString()).toBe("/(?:)/");
    expect(RegExp(undefined).toString()).toBe("/(?:)/");
    expect(RegExp("foo").toString()).toBe("/foo/");
    expect(RegExp("foo", undefined).toString()).toBe("/foo/");
    expect(RegExp("foo", "g").toString()).toBe("/foo/g");
    expect(RegExp(undefined, "g").toString()).toBe("/(?:)/g");
});

test("regexp object as pattern parameter", () => {
    expect(RegExp(/foo/).toString()).toBe("/foo/");
    expect(RegExp(/foo/g).toString()).toBe("/foo/g");
    expect(RegExp(/foo/g, "").toString()).toBe("/foo/");
    expect(RegExp(/foo/g, "y").toString()).toBe("/foo/y");

    var regex_like_object_without_flags = {
        source: "foo",
        [Symbol.match]: function () {},
    };
    expect(RegExp(regex_like_object_without_flags).toString()).toBe("/foo/");
    expect(RegExp(regex_like_object_without_flags, "y").toString()).toBe("/foo/y");

    var regex_like_object_with_flags = {
        source: "foo",
        flags: "g",
        [Symbol.match]: function () {},
    };
    expect(RegExp(regex_like_object_with_flags).toString()).toBe("/foo/g");
    expect(RegExp(regex_like_object_with_flags, "").toString()).toBe("/foo/");
    expect(RegExp(regex_like_object_with_flags, "y").toString()).toBe("/foo/y");
});

test("regexp literals are re-useable", () => {
    for (var i = 0; i < 2; ++i) {
        const re = /test/;
        expect(re.test("te")).toBeFalse();
        expect(re.test("test")).toBeTrue();
    }
});

test("Incorrectly escaped code units not converted to invalid patterns", () => {
    const re = /[\⪾-\⫀]/;
    expect(re.test("⫀")).toBeTrue();
    expect(re.test("\\u2abe")).toBeFalse(); // ⫀ is \u2abe
});

test("regexp that always matches stops matching if it's past the end of the string instead of infinitely looping", () => {
    const re = new RegExp("[\u200E]*", "gu");
    expect("whf".match(re)).toEqual(["", "", "", ""]);
    expect(re.lastIndex).toBe(0);
});

test("v flag should enable unicode mode", () => {
    const re = new RegExp("a\\u{10FFFF}", "v");
    expect(re.test("a\u{10FFFF}")).toBe(true);
});

test("parsing a large bytestring shouldn't crash", () => {
    RegExp(new Uint8Array(0x40000));
});

test("Unicode non-ASCII matching", () => {
    const cases = [
        { pattern: /é/u, match: "é", expected: ["é"] },
        { pattern: /é/, match: "é", expected: ["é"] },
        { pattern: /\u{61}/u, match: "a", expected: ["a"] },
        { pattern: /\u{61}/, match: "a", expected: null },
        { pattern: /😄/u, match: "😄", expected: ["😄"] },
        { pattern: /😄/u, match: "\ud83d", expected: null },
        { pattern: /😄/, match: "\ud83d", expected: null },
    ];
    for (const test of cases) {
        const result = test.match.match(test.pattern);
        expect(result).toEqual(test.expected);
    }
});

// https://github.com/tc39/test262/tree/main/test/built-ins/RegExp/unicodeSets/generated
test("Unicode properties of strings", () => {
    const regexes = [
        /\p{Basic_Emoji}/v,
        /\p{Emoji_Keycap_Sequence}/v,
        /\p{RGI_Emoji_Modifier_Sequence}/v,
        /\p{RGI_Emoji_Flag_Sequence}/v,
        /\p{RGI_Emoji_Tag_Sequence}/v,
        /\p{RGI_Emoji_ZWJ_Sequence}/v,
        /\p{RGI_Emoji}/v,
    ];

    for (const re of regexes) {
        expect(() => {
            re.test("test");
        }).not.toThrow();
    }

    function testExtendedCharacterClass({ regExp, matchStrings, nonMatchStrings }) {
        matchStrings.forEach(str => expect(regExp.test(str)).toBeTrue());
        nonMatchStrings.forEach(str => expect(regExp.test(str)).toBeFalse());
    }

    testExtendedCharacterClass({
        regExp: /^[\p{ASCII_Hex_Digit}--\p{Emoji_Keycap_Sequence}]+$/v,
        matchStrings: ["0", "1", "2", "3", "4", "5", "8", "A", "B", "D", "E", "F", "a", "b", "c", "d", "e", "f"],
        nonMatchStrings: [
            "6\uFE0F\u20E3",
            "7\uFE0F\u20E3",
            "9\uFE0F\u20E3",
            "\u2603",
            "\u{1D306}",
            "\u{1F1E7}\u{1F1EA}",
        ],
    });

    testExtendedCharacterClass({
        regExp: /^[\d\p{Emoji_Keycap_Sequence}]+$/v,
        matchStrings: ["#\uFE0F\u20E3", "*\uFE0F\u20E3", "0", "0\uFE0F\u20E3", "9", "9\uFE0F\u20E3"],
        nonMatchStrings: ["C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[[0-9]\p{Emoji_Keycap_Sequence}]+$/v,
        matchStrings: ["#\uFE0F\u20E3", "*\uFE0F\u20E3", "0", "0\uFE0F\u20E3", "9", "9\uFE0F\u20E3"],
        nonMatchStrings: ["C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[_--[0-9]]+$/v,
        matchStrings: ["_"],
        nonMatchStrings: ["6\uFE0F\u20E3", "7", "9\uFE0F\u20E3", "C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\p{ASCII_Hex_Digit}--[0-9]]+$/v,
        matchStrings: ["a", "b"],
        nonMatchStrings: ["0", "9", "9\uFE0F\u20E3", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\p{ASCII_Hex_Digit}\p{Emoji_Keycap_Sequence}]+$/v,
        matchStrings: ["#\uFE0F\u20E3", "*\uFE0F\u20E3", "0", "0\uFE0F\u20E3", "A", "B", "a", "b"],
        nonMatchStrings: ["\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[_\p{Emoji_Keycap_Sequence}]+$/v,
        matchStrings: ["#\uFE0F\u20E3", "*\uFE0F\u20E3", "0\uFE0F\u20E3", "_"],
        nonMatchStrings: ["7", "C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\p{Emoji_Keycap_Sequence}--\d]+$/v,
        matchStrings: ["#\uFE0F\u20E3", "*\uFE0F\u20E3", "0\uFE0F\u20E3"],
        nonMatchStrings: ["7", "C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\p{Emoji_Keycap_Sequence}--[0-9]]+$/v,
        matchStrings: ["#\uFE0F\u20E3", "*\uFE0F\u20E3", "0\uFE0F\u20E3"],
        nonMatchStrings: ["7", "C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\p{Emoji_Keycap_Sequence}--\p{ASCII_Hex_Digit}]+$/v,
        matchStrings: ["#\uFE0F\u20E3", "*\uFE0F\u20E3", "0\uFE0F\u20E3"],
        nonMatchStrings: ["7", "C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\p{Emoji_Keycap_Sequence}--_]+$/v,
        matchStrings: ["#\uFE0F\u20E3", "*\uFE0F\u20E3", "0\uFE0F\u20E3"],
        nonMatchStrings: ["7", "C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\p{Emoji_Keycap_Sequence}&&\p{Emoji_Keycap_Sequence}]+$/v,
        matchStrings: ["#\uFE0F\u20E3", "*\uFE0F\u20E3", "0\uFE0F\u20E3"],
        nonMatchStrings: ["7", "C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\p{Emoji_Keycap_Sequence}\d]+$/v,
        matchStrings: ["#\uFE0F\u20E3", "*\uFE0F\u20E3", "0", "0\uFE0F\u20E3", "9", "9\uFE0F\u20E3"],
        nonMatchStrings: ["C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\p{Emoji_Keycap_Sequence}[0-9]]+$/v,
        matchStrings: ["#\uFE0F\u20E3", "*\uFE0F\u20E3", "0", "0\uFE0F\u20E3", "9", "9\uFE0F\u20E3"],
        nonMatchStrings: ["C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\p{Emoji_Keycap_Sequence}\p{ASCII_Hex_Digit}]+$/v,
        matchStrings: ["#\uFE0F\u20E3", "*\uFE0F\u20E3", "0", "0\uFE0F\u20E3", "9", "9\uFE0F\u20E3", "A", "a"],
        nonMatchStrings: ["\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\p{Emoji_Keycap_Sequence}_]+$/v,
        matchStrings: ["#\uFE0F\u20E3", "*\uFE0F\u20E3", "0\uFE0F\u20E3", "_"],
        nonMatchStrings: ["7", "C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\p{Emoji_Keycap_Sequence}\p{Emoji_Keycap_Sequence}]+$/v,
        matchStrings: ["#\uFE0F\u20E3", "*\uFE0F\u20E3", "0\uFE0F\u20E3"],
        nonMatchStrings: ["7", "C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\d--\q{0|2|4|9\uFE0F\u20E3}]+$/v,
        expression: "[\d--\q{0|2|4|9\uFE0F\u20E3}]",
        matchStrings: ["1", "9"],
        nonMatchStrings: ["0", "9\uFE0F\u20E3", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\d&&\q{0|2|4|9\uFE0F\u20E3}]+$/v,
        expression: "[\d&&\q{0|2|4|9\uFE0F\u20E3}]",
        matchStrings: ["0", "2", "4"],
        nonMatchStrings: ["1", "9\uFE0F\u20E3", "C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\d\q{0|2|4|9\uFE0F\u20E3}]+$/v,
        expression: "[\d\q{0|2|4|9\uFE0F\u20E3}]",
        matchStrings: ["0", "9\uFE0F\u20E3"],
        nonMatchStrings: ["6\uFE0F\u20E3", "C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\p{Emoji_Keycap_Sequence}--\q{0|2|4|9\uFE0F\u20E3}]+$/v,
        expression: "[\p{Emoji_Keycap_Sequence}--\q{0|2|4|9\uFE0F\u20E3}]",
        matchStrings: ["#\uFE0F\u20E3", "8\uFE0F\u20E3"],
        nonMatchStrings: ["7", "9\uFE0F\u20E3", "C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\p{Emoji_Keycap_Sequence}\q{0|2|4|9\uFE0F\u20E3}]+$/v,
        expression: "[\p{Emoji_Keycap_Sequence}\q{0|2|4|9\uFE0F\u20E3}]",
        matchStrings: ["#\uFE0F\u20E3", "0", "9\uFE0F\u20E3"],
        nonMatchStrings: ["7", "C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\q{0|2|4|9\uFE0F\u20E3}--\q{0|2|4|9\uFE0F\u20E3}]+$/v,
        expression: "[\q{0|2|4|9\uFE0F\u20E3}--\q{0|2|4|9\uFE0F\u20E3}]",
        matchStrings: [],
        nonMatchStrings: ["0", "9\uFE0F\u20E3", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\q{0|2|4|9\uFE0F\u20E3}&&\q{0|2|4|9\uFE0F\u20E3}]+$/v,
        expression: "[\q{0|2|4|9\uFE0F\u20E3}&&\q{0|2|4|9\uFE0F\u20E3}]",
        matchStrings: ["0", "2", "4", "9\uFE0F\u20E3"],
        nonMatchStrings: ["6\uFE0F\u20E3", "7", "C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\q{0|2|4|9\uFE0F\u20E3}\q{0|2|4|9\uFE0F\u20E3}]+$/v,
        expression: "[\q{0|2|4|9\uFE0F\u20E3}\q{0|2|4|9\uFE0F\u20E3}]",
        matchStrings: ["0", "2", "4", "9\uFE0F\u20E3"],
        nonMatchStrings: ["6\uFE0F\u20E3", "7", "C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });

    testExtendedCharacterClass({
        regExp: /^[\q{0|2|4|9\uFE0F\u20E3}&&\p{Emoji_Keycap_Sequence}]+$/v,
        expression: "[\q{0|2|4|9\uFE0F\u20E3}&&\p{Emoji_Keycap_Sequence}]",
        matchStrings: ["9\uFE0F\u20E3"],
        nonMatchStrings: ["0", "2", "4", "6\uFE0F\u20E3", "7", "C", "\u2603", "\u{1D306}", "\u{1F1E7}\u{1F1EA}"],
    });
});

test("Unicode matching with u and v flags", () => {
    const text = "𠮷a𠮷b𠮷";
    const complexText = "a\u{20BB7}b\u{10FFFF}c";

    const cases = [
        { pattern: /𠮷/, match: text, expected: ["𠮷"] },
        { pattern: /𠮷/u, match: text, expected: ["𠮷"] },
        { pattern: /𠮷/v, match: text, expected: ["𠮷"] },
        { pattern: /\p{Script=Han}/u, match: text, expected: ["𠮷"] },
        { pattern: /\p{Script=Han}/v, match: text, expected: ["𠮷"] },
        { pattern: /./u, match: text, expected: ["𠮷"] },
        { pattern: /./v, match: text, expected: ["𠮷"] },
        { pattern: /\p{ASCII}/u, match: text, expected: ["a"] },
        { pattern: /\p{ASCII}/v, match: text, expected: ["a"] },
        { pattern: /x/u, match: text, expected: null },
        { pattern: /x/v, match: text, expected: null },
        { pattern: /\p{Script=Han}(.)/gu, match: text, expected: ["𠮷a", "𠮷b"] },
        { pattern: /\p{Script=Han}(.)/gv, match: text, expected: ["𠮷a", "𠮷b"] },
        { pattern: /\P{ASCII}/u, match: complexText, expected: ["\u{20BB7}"] },
        { pattern: /\P{ASCII}/v, match: complexText, expected: ["\u{20BB7}"] },
        { pattern: /\P{ASCII}/gu, match: complexText, expected: ["\u{20BB7}", "\u{10FFFF}"] },
        { pattern: /\P{ASCII}/gv, match: complexText, expected: ["\u{20BB7}", "\u{10FFFF}"] },
        { pattern: /./gu, match: text, expected: ["𠮷", "a", "𠮷", "b", "𠮷"] },
        { pattern: /./gv, match: text, expected: ["𠮷", "a", "𠮷", "b", "𠮷"] },
        { pattern: /(?:)/gu, match: text, expected: ["", "", "", "", "", ""] },
        { pattern: /(?:)/gv, match: text, expected: ["", "", "", "", "", ""] },
        // Character class splits family emoji (👨‍👩‍👧‍👦) into individual components, so it should match only the first one (👨)
        { pattern: /[👨‍👩‍👧‍👦]/v, match: "𠮷a𠮷b𠮷c👨‍👩‍👧‍👦d", expected: ["👨"] },
    ];

    for (const test of cases) {
        const result = test.match.match(test.pattern);
        expect(result).toEqual(test.expected);
    }
});

test("RegExp string literal", () => {
    [
        { pattern: /[\q{abc}]/v, match: "abc", expected: ["abc"] },
        { pattern: /[\q{abc}]/v, match: "a", expected: null },
        { pattern: /[\q{a|b}]/v, match: "b", expected: ["b"] },
        { pattern: /[\q{a\\b}]/v, match: "a\\b", expected: ["a\\b"] },
        { pattern: /[\q{}]/v, match: "", expected: [""] },
        { pattern: /[\q{😀|😁|😂}]/v, match: "😁", expected: ["😁"] },
        { pattern: /[\q{1|1\uFE0F\u20E3}]/v, match: "1️⃣", expected: ["1️⃣"] },
        { pattern: /[\q{1}]/v, match: "1️⃣", expected: ["1"] },
        { pattern: /[\d&&\q{2}]/v, match: "123", expected: ["2"] },
        { pattern: /[^\q{a|b}]/v, match: "abc", expected: ["c"] },
        { pattern: /[\q{\n}]/v, match: "\n", expected: ["\n"] },
        { pattern: /[\q{\b}]/v, match: "\b", expected: ["\b"] },
        { pattern: /[\q{\0}]/v, match: "\0", expected: ["\0"] },
        { pattern: /[\q{\|}]/v, match: "|", expected: ["|"] },
        { pattern: /[\q{\x41}]/v, match: "A", expected: ["A"] },
        {
            pattern: /[\q{\uD83D\uDC68\u200d\uD83D\uDC69\u200d\uD83D\uDC66\u200d\uD83D\uDC66}]/v,
            match: "👨‍👩‍👦‍👦",
            expected: ["👨‍👩‍👦‍👦"],
        },
        { pattern: /[\q{\u{1F600}}]/v, match: "😀", expected: ["😀"] },
        { pattern: /[\q{\cZ}]/v, match: "\x1A", expected: ["\x1A"] },
        { pattern: /[\q{  }]/v, match: "  ", expected: ["  "] },
        { pattern: /[[\d+]--[\q{1}]]/gv, match: "12", expected: ["2"] },
        { pattern: /[[\d]&&[\q{1}]]/gv, match: "21", expected: ["1"] },
        { pattern: /[\d\q{a}]/gv, match: "a1", expected: ["a", "1"] },
    ].forEach(test => {
        const result = test.match.match(test.pattern);
        expect(result).toEqual(test.expected);
    });

    [
        "[\\q{(a)}]",
        "[\\q{[a]}]",
        "[\\q{{a}}]",
        "[^\\q{bad}]",
        "[\\q{a-b}]",
        "[^\\q{a|bc}]",
        "[^\\q{\\b+}]",
        "[\\q{\\d}]",
        "[\\q{\\w}]",
        "[\\q{\\q}]",
        "[^\\q{\\(\\)}]",
    ].forEach(pattern => {
        expect(() => new RegExp(pattern, "v")).toThrow(SyntaxError);
    });
});

// https://github.com/tc39/test262/tree/main/test/built-ins/RegExp/regexp-modifiers
test("RegExp modifiers", () => {
    const testModifiers = (pattern, flags, tests) => {
        const re = new RegExp(pattern, flags);
        tests.forEach(([input, expected]) => expect(re.test(input)).toBe(expected));
    };

    testModifiers("(^a$)|(?:^b$)|(?m:^c$)|(?:^d$)|(^e$)", "", [
        ["\na\n", false],
        ["\nb\n", false],
        ["\nc\n", true],
        ["\nd\n", false],
        ["\ne\n", false],
    ]);

    testModifiers("(?m-:es$|(?-m:js$))", "", [
        ["es\ns", true],
        ["js", true],
        ["js\ns", false],
    ]);

    testModifiers("(a)|(?:b)|(?-i:c)|(?:d)|(e)", "i", [
        ["A", true],
        ["B", true],
        ["C", false],
        ["D", true],
        ["E", true],
    ]);

    testModifiers("(?m:es.$)", "", [
        ["esz\n", true],
        ["es\n\n", false],
    ]);

    testModifiers("(?m-:es.$)", "s", [
        ["esz\n", true],
        ["es\n\n", true],
    ]);

    testModifiers("(?-i:\\u{0061})b", "iu", [
        ["ab", true],
        ["aB", true],
        ["Ab", false],
    ]);

    testModifiers("(?-i:\\p{Lu})", "iu", [
        ["A", true],
        ["a", false],
        ["Z", true],
        ["z", false],
    ]);

    testModifiers("(?-m:^es)$", "m", [
        ["e\nes\n", false],
        ["es\n", true],
    ]);
});

test("Unicode case-insensitive matching", () => {
    const testMatch = (pattern, string, expected) => {
        const result = string.match(pattern);
        expect(result).toEqual(expected);
    };

    // U+017F - Latin Small Letter Long S (ſ)
    testMatch(/\w/iv, "\u017F", ["\u017F"]);
    testMatch(/\W/iv, "\u017F", null);

    // U+212A - Kelvin Sign (K)
    testMatch(/\b/i, "\u017F", null);
    testMatch(/\b/iv, "\u017F", [""]);
    testMatch(/\b/i, "\u212A", null);
    testMatch(/\b/iv, "\u212A", [""]);

    // ß shouldn't expand to SS
    testMatch(/ss/i, "ß", null);
    testMatch(/ss/iv, "ß", null);

    // Greek Sigma has three case forms (Σ, σ, ς)
    testMatch(/ς/i, "Σ", ["Σ"]);
    testMatch(/ς/i, "σ", ["σ"]);
    testMatch(/ς/i, "ς", ["ς"]);
    testMatch(/Σ/i, "Σ", ["Σ"]);
    testMatch(/Σ/i, "σ", ["σ"]);
    testMatch(/Σ/i, "ς", ["ς"]);

    // Accented characters
    testMatch(/Ï/, "ï", null);
    testMatch(/Ï/i, "ï", ["ï"]);
    testMatch(/á/i, "Á", ["Á"]);
    testMatch(/[á]/i, "Á", ["Á"]);
    testMatch(/[á-á]/i, "Á", ["Á"]);
    testMatch(/être/i, "ÊTRE", ["ÊTRE"]);
    testMatch(/[être]/i, "ÊTRE", ["Ê"]);

    // Uppercase (Lu) and Lowercase (Ll)
    testMatch(/\p{Lu}/v, "ẞ", ["ẞ"]);
    testMatch(/\p{Lu}/iv, "ẞ", ["ẞ"]);
    testMatch(/\p{Ll}/v, "ẞ", null);
    testMatch(/\p{Ll}/iv, "ẞ", ["ẞ"]);

    testMatch(/\p{Lu}/v, "ß", null);
    testMatch(/\p{Lu}/iv, "ß", ["ß"]);
    testMatch(/\p{Ll}/v, "ß", ["ß"]);
    testMatch(/\p{Ll}/iv, "ß", ["ß"]);

    testMatch(/\p{Lu}/v, "Σ", ["Σ"]);
    testMatch(/\p{Lu}/iv, "Σ", ["Σ"]);
    testMatch(/\p{Lu}/v, "σ", null);
    testMatch(/\p{Lu}/iv, "σ", ["σ"]);
    testMatch(/\p{Lu}/v, "ς", null);
    testMatch(/\p{Lu}/iv, "ς", ["ς"]);

    testMatch(/\p{Lu}/gv, "Áá", ["Á"]);
    testMatch(/\p{Lu}/giv, "Áá", ["Á", "á"]);
    testMatch(/\p{Ll}/gv, "Áá", ["á"]);
    testMatch(/\p{Ll}/giv, "Áá", ["Á", "á"]);
    testMatch(/\p{Lu}/gv, "i\u0307", null);
    testMatch(/\p{Lu}/giv, "i\u0307", ["i"]);

    testMatch(/\p{Ll}/giu, "Aa", ["A", "a"]);
    testMatch(/[^\P{Ll}]/giu, "Aa", null);

    testMatch(/[\p{Ll}]/giv, "Aa", ["A", "a"]);
    testMatch(/[^\P{Ll}]/giv, "Aa", ["A", "a"]);

    testMatch(/\P{Ll}/giu, "Aa", ["A", "a"]);
    testMatch(/\P{Ll}/giv, "Aa", null);
    testMatch(/\P{Lu}/giu, "Aa", ["A", "a"]);
    testMatch(/\P{Lu}/giv, "Aa", null);

    testMatch(/[[\p{Ll}&&\p{Lu}]á]/i, "Á", null);
    testMatch(/[[\p{Ll}&&\p{Lu}]á]/iv, "Á", ["Á"]);

    // Binary properties
    testMatch(/\p{Uppercase}/gv, "Áá", ["Á"]);
    testMatch(/\p{Uppercase}/giv, "Áá", ["Á", "á"]);
    testMatch(/\p{Lowercase}/gv, "Áá", ["á"]);
    testMatch(/\p{Lowercase}/giv, "Áá", ["Á", "á"]);

    // String literals
    testMatch(/[á\q{ábc}]/giv, "ÁÁBC", ["Á", "ÁBC"]);
    testMatch(/[á\q{ábc}]/giv, "áBC", ["áBC"]);

    // U+FB05 - Latin Small Ligature Long S T (ﬅ)
    testMatch(/[\ufb05]/i, "\ufb06", null);
    testMatch(/[\ufb05]/v, "\ufb06", null);
    testMatch(/[\ufb05]/iv, "\ufb06", ["ﬆ"]);

    // U+FB06 - Latin Small Ligature ST (ﬆ)
    testMatch(/[\ufb06]/i, "\ufb05", null);
    testMatch(/[\ufb06]/v, "\ufb05", null);
    testMatch(/[\ufb06]/iv, "\ufb05", ["ﬅ"]);

    // Greek lowercase letters
    testMatch(/[\u0390]/iv, "\u1fd3", ["\u1fd3"]);
    testMatch(/[\u1fd3]/iv, "\u0390", ["\u0390"]);
    testMatch(/[\u03b0]/iv, "\u1fe3", ["\u1fe3"]);
    testMatch(/[\u1fe3]/iv, "\u03b0", ["\u03b0"]);

    // U+017F - Latin Small Letter Long S (ſ)
    testMatch(/[a-z]/i, "\u017F", null);
    testMatch(/[a-z]/iv, "\u017F", ["\u017F"]);
    testMatch(/s/i, "\u017F", null);
    testMatch(/s/iv, "\u017F", ["\u017F"]);

    // U+212A - Kelvin Sign (K)
    testMatch(/[a-z]/i, "\u212A", null);
    testMatch(/[a-z]/iv, "\u212A", ["\u212A"]);
    testMatch(/k/i, "\u212A", null);
    testMatch(/k/iv, "\u212A", ["\u212A"]);

    // U+2126 - Ohm Sign (Ω)
    testMatch(/[ω]/i, "\u2126", null);
    testMatch(/[ω]/iv, "\u2126", ["\u2126"]);
    testMatch(/[\u03A9]/i, "\u2126", null);
    testMatch(/[\u03A9]/iv, "\u2126", ["\u2126"]);
});
