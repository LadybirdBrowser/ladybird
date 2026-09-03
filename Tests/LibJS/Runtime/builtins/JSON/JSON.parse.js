test("basic functionality", () => {
    expect(JSON.parse).toHaveLength(2);

    const properties = [
        ["5", 5],
        ["null", null],
        ["true", true],
        ["false", false],
        ['"test"', "test"],
        ['[1,2,"foo"]', [1, 2, "foo"]],
        ['{"foo":1,"bar":"baz"}', { foo: 1, bar: "baz" }],
    ];

    properties.forEach(testCase => {
        expect(JSON.parse(testCase[0])).toEqual(testCase[1]);
    });
});

test("syntax errors", () => {
    [
        undefined,
        NaN,
        -NaN,
        Infinity,
        -Infinity,
        '{ "foo" }',
        '{ foo: "bar" }',
        "[1,2,3,]",
        "[1,2,3, ]",
        '{ "foo": "bar",}',
        '{ "foo": "bar", }',
        "",
    ].forEach(test => {
        expect(() => {
            JSON.parse(test);
        }).toThrow(SyntaxError);
    });
});

test("negative zero", () => {
    ["-0", " \n-0", "-0  \t", "\n\t -0\n   ", "-0.0"].forEach(testCase => {
        expect(JSON.parse(testCase)).toEqual(-0.0);
    });

    expect(JSON.parse(-0)).toEqual(0);
});

// The underlying parser resolves decimal numbers by storing the decimal portion in an integer
// This test handles a regression where the decimal portion was only using a u32 vs. u64
// and would fail to parse.
test("long decimal parse", () => {
    expect(JSON.parse("1644452550.6489999294281")).toEqual(1644452550.6489999294281);
});

test("does not truncate large integers", () => {
    expect(JSON.parse("1234567890123")).toEqual(1234567890123);
    expect(JSON.parse("4294967295")).toEqual(4294967295);
    expect(JSON.parse("4294967296")).toEqual(4294967296);
    expect(JSON.parse("4294967297")).toEqual(4294967297);
    expect(JSON.parse("4294967298")).toEqual(4294967298);

    expect(JSON.parse("2147483647")).toEqual(2147483647);
    expect(JSON.parse("2147483648")).toEqual(2147483648);
    expect(JSON.parse("2147483649")).toEqual(2147483649);
    expect(JSON.parse("2147483650")).toEqual(2147483650);

    expect(JSON.parse("9007199254740991")).toEqual(9007199254740991);
    expect(JSON.parse("9007199254740992")).toEqual(9007199254740992);
    expect(JSON.parse("9007199254740993")).toEqual(9007199254740993);
    expect(JSON.parse("9007199254740994")).toEqual(9007199254740994);
    expect(JSON.parse("9008199254740994")).toEqual(9008199254740994);

    expect(JSON.parse("18446744073709551615")).toEqual(18446744073709551615);
    expect(JSON.parse("18446744073709551616")).toEqual(18446744073709551616);
    expect(JSON.parse("18446744073709551617")).toEqual(18446744073709551617);
});

test("number overflow to infinity", () => {
    expect(JSON.parse("1e309")).toBe(Infinity);
    expect(JSON.parse("-1e309")).toBe(-Infinity);
    expect(JSON.parse("1e-400")).toBe(0);
});

test("rejects invalid number formats", () => {
    // Leading zeros not allowed
    expect(() => JSON.parse("01")).toThrow(SyntaxError);
    expect(() => JSON.parse("-01")).toThrow(SyntaxError);
    expect(() => JSON.parse("00")).toThrow(SyntaxError);
    expect(() => JSON.parse("007")).toThrow(SyntaxError);

    // Trailing decimal point not allowed
    expect(() => JSON.parse("1.")).toThrow(SyntaxError);
    expect(() => JSON.parse("0.")).toThrow(SyntaxError);
    expect(() => JSON.parse("-1.")).toThrow(SyntaxError);

    // Other invalid formats
    expect(() => JSON.parse("+1")).toThrow(SyntaxError);
    expect(() => JSON.parse(".1")).toThrow(SyntaxError);
    expect(() => JSON.parse("1e")).toThrow(SyntaxError);
    expect(() => JSON.parse("1e+")).toThrow(SyntaxError);
    expect(() => JSON.parse("1e-")).toThrow(SyntaxError);
});

test("rejects trailing content", () => {
    expect(() => JSON.parse("123 garbage")).toThrow(SyntaxError);
    expect(() => JSON.parse("null garbage")).toThrow(SyntaxError);
    expect(() => JSON.parse("true garbage")).toThrow(SyntaxError);
    expect(() => JSON.parse('"string" garbage')).toThrow(SyntaxError);
    expect(() => JSON.parse("[] garbage")).toThrow(SyntaxError);
    expect(() => JSON.parse("{} garbage")).toThrow(SyntaxError);
});

test("string escape sequences", () => {
    expect(JSON.parse('"\\""')).toBe('"');
    expect(JSON.parse('"\\\\"')).toBe("\\");
    expect(JSON.parse('"\\/"')).toBe("/");
    expect(JSON.parse('"\\b"')).toBe("\b");
    expect(JSON.parse('"\\f"')).toBe("\f");
    expect(JSON.parse('"\\n"')).toBe("\n");
    expect(JSON.parse('"\\r"')).toBe("\r");
    expect(JSON.parse('"\\t"')).toBe("\t");
    expect(JSON.parse('"\\u0041"')).toBe("A");
    expect(JSON.parse('"\\u0000"')).toBe("\0");
});

test("unicode and surrogate pairs", () => {
    expect(JSON.parse('"café"')).toBe("café");
    expect(JSON.parse('"日本語"')).toBe("日本語");
    expect(JSON.parse('"\\uD83D\\uDE00"')).toBe("😀");
    expect(JSON.parse('"\\u4e2d\\u6587"')).toBe("中文");

    // Lone surrogates (valid JSON)
    expect(JSON.parse('"\\uD800"')).toBe("\uD800");
    expect(JSON.parse('"\\uDFFF"')).toBe("\uDFFF");

    const actualHighSurrogate = JSON.parse('"\uD800"');
    expect(actualHighSurrogate).toHaveLength(1);
    expect(actualHighSurrogate.charCodeAt(0)).toBe(0xd800);

    const actualLowSurrogate = JSON.parse('"\uDC00"');
    expect(actualLowSurrogate).toHaveLength(1);
    expect(actualLowSurrogate.charCodeAt(0)).toBe(0xdc00);

    expect(JSON.parse('{"\uD800":"\uDC00"}')["\uD800"].charCodeAt(0)).toBe(0xdc00);
});

test("lone surrogates among non-ASCII text", () => {
    const mixed = JSON.parse('"日\uD800本\uDC00語"');
    expect(mixed).toHaveLength(5);
    expect(mixed.charCodeAt(1)).toBe(0xd800);
    expect(mixed.charCodeAt(3)).toBe(0xdc00);
    expect(mixed[4]).toBe("語");

    const consecutive = JSON.parse('["\uD800\uD800", "\uDC00\uDC00", "é\uDC00"]');
    expect(consecutive[0]).toHaveLength(2);
    expect(consecutive[0].charCodeAt(1)).toBe(0xd800);
    expect(consecutive[1]).toHaveLength(2);
    expect(consecutive[1].charCodeAt(0)).toBe(0xdc00);
    expect(consecutive[2]).toBe("é\uDC00");

    expect(JSON.parse('{"日\uD800":"\uD83D\uDE00"}')["日\uD800"]).toBe("😀");
    expect(() => JSON.parse("\uD800")).toThrow(SyntaxError);
});

test("object keys repeated across many objects", () => {
    const text = JSON.stringify(
        Array.from({ length: 300 }, (_, i) => ({ title: "やること " + i, completed: i % 2 === 0, id: "id" + i }))
    );
    const parsed = JSON.parse(text);
    expect(parsed).toHaveLength(300);
    expect(parsed[299]).toEqual({ title: "やること 299", completed: false, id: "id299" });
    expect(Object.keys(parsed[7])).toEqual(["title", "completed", "id"]);

    const similar = JSON.parse('[{"abc":1,"aXc":2,"abd":3},{"abc":4,"aXc":5,"abd":6}]');
    expect(similar[1]).toEqual({ abc: 4, aXc: 5, abd: 6 });

    const numeric = JSON.parse('[{"1":"a","01":"b","x":"c"},{"1":"d","01":"e","x":"f"}]');
    expect(Object.keys(numeric[1])).toEqual(["1", "01", "x"]);
    expect(numeric[1][1]).toBe("d");

    const longKey = "k".repeat(40);
    expect(JSON.parse(`[{"${longKey}":1},{"${longKey}":2}]`)[1][longKey]).toBe(2);

    expect(JSON.parse('{"a":1,"\\u0061":2}')).toEqual({ a: 2 });
    expect(JSON.parse('{"":1,"":2}')).toEqual({ "": 2 });
});

test("whitespace handling", () => {
    expect(JSON.parse(" null")).toBe(null);
    expect(JSON.parse("null ")).toBe(null);
    expect(JSON.parse(" null ")).toBe(null);
    expect(JSON.parse("\t123")).toBe(123);
    expect(JSON.parse("123\n")).toBe(123);
    expect(JSON.parse("\r\n123\r\n")).toBe(123);
    expect(JSON.parse("  {  }  ")).toEqual({});
    expect(JSON.parse("  [  ]  ")).toEqual([]);
});
