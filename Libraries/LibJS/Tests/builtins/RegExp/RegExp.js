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
