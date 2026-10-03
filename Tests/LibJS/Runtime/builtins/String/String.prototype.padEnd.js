test("basic functionality", () => {
    expect(String.prototype.padEnd).toHaveLength(1);

    var s = "foo";
    expect(s.padEnd(-1)).toBe("foo");
    expect(s.padEnd(0)).toBe("foo");
    expect(s.padEnd(3)).toBe("foo");
    expect(s.padEnd(5)).toBe("foo  ");
    expect(s.padEnd(10)).toBe("foo       ");
    expect(s.padEnd("5")).toBe("foo  ");
    expect(s.padEnd([[["5"]]])).toBe("foo  ");
    expect(s.padEnd(2, "+")).toBe("foo");
    expect(s.padEnd(5, "+")).toBe("foo++");
    expect(s.padEnd(5, 1)).toBe("foo11");
    expect(s.padEnd(10, null)).toBe("foonullnul");
    expect(s.padEnd(10, "bar")).toBe("foobarbarb");
    expect(s.padEnd(10, "123456789")).toBe("foo1234567");
});

test("UTF-16", () => {
    var s = "😀";
    expect(s).toHaveLength(2);
    expect(s.padEnd(-1)).toBe("😀");
    expect(s.padEnd(0)).toBe("😀");
    expect(s.padEnd(1)).toBe("😀");
    expect(s.padEnd(2)).toBe("😀");
    expect(s.padEnd(3)).toBe("😀 ");
    expect(s.padEnd(10)).toBe("😀        ");

    expect(s.padEnd(2, "😀")).toBe("😀");
    expect(s.padEnd(3, "😀")).toBe("😀\ud83d");
    expect(s.padEnd(4, "😀")).toBe("😀😀");
    expect(s.padEnd(5, "😀")).toBe("😀😀\ud83d");
});

test("result longer than the maximum string length", () => {
    expect(() => {
        "a".padEnd(2 ** 32);
    }).toThrowWithMessage(RangeError, "string size must not overflow");
    expect(() => {
        "a".padEnd(2 ** 53 - 1, "xyz");
    }).toThrowWithMessage(RangeError, "string size must not overflow");
    expect("a".padEnd(2 ** 53 - 1, "")).toBe("a");
});
