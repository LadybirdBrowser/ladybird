test("basic functionality", () => {
    expect(String.prototype.charAt).toHaveLength(1);

    var s = "foobar";
    expect(typeof s).toBe("string");
    expect(s).toHaveLength(6);

    expect(s.charAt(0)).toBe("f");
    expect(s.charAt(1)).toBe("o");
    expect(s.charAt(2)).toBe("o");
    expect(s.charAt(3)).toBe("b");
    expect(s.charAt(4)).toBe("a");
    expect(s.charAt(5)).toBe("r");
    expect(s.charAt(6)).toBe("");

    expect(s.charAt()).toBe("f");
    expect(s.charAt(NaN)).toBe("f");
    expect(s.charAt("foo")).toBe("f");
    expect(s.charAt(undefined)).toBe("f");
});

test("UTF-16", () => {
    var s = "😀";
    expect(s).toHaveLength(2);
    expect(s.charAt(0)).toBe("\ud83d");
    expect(s.charAt(1)).toBe("\ude00");
    expect(s.charAt(2)).toBe("");
});
