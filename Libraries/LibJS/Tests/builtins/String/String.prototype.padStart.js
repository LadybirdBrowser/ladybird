test("basic functionality", () => {
    expect(String.prototype.padStart).toHaveLength(1);

    var s = "foo";
    expect(s.padStart(-1)).toBe("foo");
    expect(s.padStart(0)).toBe("foo");
    expect(s.padStart(3)).toBe("foo");
    expect(s.padStart(5)).toBe("  foo");
    expect(s.padStart(10)).toBe("       foo");
    expect(s.padStart("5")).toBe("  foo");
    expect(s.padStart([[["5"]]])).toBe("  foo");
    expect(s.padStart(2, "+")).toBe("foo");
    expect(s.padStart(5, "+")).toBe("++foo");
    expect(s.padStart(5, 1)).toBe("11foo");
    expect(s.padStart(10, null)).toBe("nullnulfoo");
    expect(s.padStart(10, "bar")).toBe("barbarbfoo");
    expect(s.padStart(10, "123456789")).toBe("1234567foo");
});

test("UTF-16", () => {
    var s = "😀";
    expect(s).toHaveLength(2);
    expect(s.padStart(-1)).toBe("😀");
    expect(s.padStart(0)).toBe("😀");
    expect(s.padStart(1)).toBe("😀");
    expect(s.padStart(2)).toBe("😀");
    expect(s.padStart(3)).toBe(" 😀");
    expect(s.padStart(10)).toBe("        😀");

    expect(s.padStart(2, "😀")).toBe("😀");
    expect(s.padStart(3, "😀")).toBe("\ud83d😀");
    expect(s.padStart(4, "😀")).toBe("😀😀");
    expect(s.padStart(5, "😀")).toBe("😀\ud83d😀");
});
