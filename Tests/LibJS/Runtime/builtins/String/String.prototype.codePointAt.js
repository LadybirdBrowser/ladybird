test("basic functionality", () => {
    expect(String.prototype.charAt).toHaveLength(1);

    var s = "Foobar";
    expect(typeof s).toBe("string");
    expect(s).toHaveLength(6);

    expect(s.codePointAt(0)).toBe(70);
    expect(s.codePointAt(1)).toBe(111);
    expect(s.codePointAt(2)).toBe(111);
    expect(s.codePointAt(3)).toBe(98);
    expect(s.codePointAt(4)).toBe(97);
    expect(s.codePointAt(5)).toBe(114);
    expect(s.codePointAt(6)).toBe(undefined);
    expect(s.codePointAt(-1)).toBe(undefined);

    expect(s.codePointAt()).toBe(70);
    expect(s.codePointAt(NaN)).toBe(70);
    expect(s.codePointAt("foo")).toBe(70);
    expect(s.codePointAt(undefined)).toBe(70);
});

test("UTF-16", () => {
    var s = "😀";
    expect(s).toHaveLength(2);
    expect(s.codePointAt(0)).toBe(0x1f600);
    expect(s.codePointAt(1)).toBe(0xde00);
    expect(s.codePointAt(2)).toBe(undefined);
});
