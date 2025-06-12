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
