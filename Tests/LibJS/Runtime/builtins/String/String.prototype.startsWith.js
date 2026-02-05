test("basic functionality", () => {
    expect(String.prototype.startsWith).toHaveLength(1);

    var s = "foobar";
    expect(s.startsWith("f")).toBeTrue();
    expect(s.startsWith("fo")).toBeTrue();
    expect(s.startsWith("foo")).toBeTrue();
    expect(s.startsWith("foob")).toBeTrue();
    expect(s.startsWith("fooba")).toBeTrue();
    expect(s.startsWith("foobar")).toBeTrue();
    expect(s.startsWith("foobar1")).toBeFalse();
    expect(s.startsWith("f", 0)).toBeTrue();
    expect(s.startsWith("fo", 0)).toBeTrue();
    expect(s.startsWith("foo", 0)).toBeTrue();
    expect(s.startsWith("foob", 0)).toBeTrue();
    expect(s.startsWith("fooba", 0)).toBeTrue();
    expect(s.startsWith("foobar", 0)).toBeTrue();
    expect(s.startsWith("foobar1", 0)).toBeFalse();
    expect(s.startsWith("foo", [])).toBeTrue();
    expect(s.startsWith("foo", null)).toBeTrue();
    expect(s.startsWith("foo", undefined)).toBeTrue();
    expect(s.startsWith("foo", false)).toBeTrue();
    expect(s.startsWith("foo", true)).toBeFalse();
    expect(s.startsWith("foo", "foo")).toBeTrue();
    expect(s.startsWith("foo", -1)).toBeTrue();
    expect(s.startsWith("foo", 42)).toBeFalse();
    expect(s.startsWith("bar", 3)).toBeTrue();
    expect(s.startsWith("bar", "3")).toBeTrue();
    expect(s.startsWith("bar1", 3)).toBeFalse();
    expect(s.startsWith()).toBeFalse();
    expect(s.startsWith("")).toBeTrue();
    expect(s.startsWith("", 0)).toBeTrue();
    expect(s.startsWith("", 1)).toBeTrue();
    expect(s.startsWith("", -1)).toBeTrue();
    expect(s.startsWith("", 42)).toBeTrue();
});

test("UTF-16", () => {
    var s = "😀";
    expect(s.startsWith("😀")).toBeTrue();
    expect(s.startsWith("\ud83d")).toBeTrue();
    expect(s.startsWith("\ude00")).toBeFalse();
    expect(s.startsWith("a")).toBeFalse();
});
