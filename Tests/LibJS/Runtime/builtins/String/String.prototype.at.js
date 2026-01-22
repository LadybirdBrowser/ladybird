test("basic functionality", () => {
    expect(String.prototype.at).toHaveLength(1);

    const string = "abc";
    expect(string.at(0)).toBe("a");
    expect(string.at(1)).toBe("b");
    expect(string.at(2)).toBe("c");
    expect(string.at(3)).toBeUndefined();
    expect(string.at(Infinity)).toBeUndefined();
    expect(string.at(-1)).toBe("c");
    expect(string.at(-2)).toBe("b");
    expect(string.at(-3)).toBe("a");
    expect(string.at(-4)).toBeUndefined();
    expect(string.at(-Infinity)).toBeUndefined();
});

test("UTF-16", () => {
    var s = "😀";
    expect(s).toHaveLength(2);
    expect(s.at(0)).toBe("\ud83d");
    expect(s.at(1)).toBe("\ude00");
    expect(s.at(2)).toBeUndefined();
});
