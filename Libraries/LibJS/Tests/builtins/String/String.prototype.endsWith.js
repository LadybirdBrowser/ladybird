test("basic functionality", () => {
    expect(String.prototype.endsWith).toHaveLength(1);

    var s = "foobar";
    expect(s.endsWith("r")).toBeTrue();
    expect(s.endsWith("ar")).toBeTrue();
    expect(s.endsWith("bar")).toBeTrue();
    expect(s.endsWith("obar")).toBeTrue();
    expect(s.endsWith("oobar")).toBeTrue();
    expect(s.endsWith("foobar")).toBeTrue();
    expect(s.endsWith("1foobar")).toBeFalse();
    expect(s.endsWith("r", 6)).toBeTrue();
    expect(s.endsWith("ar", 6)).toBeTrue();
    expect(s.endsWith("bar", 6)).toBeTrue();
    expect(s.endsWith("obar", 6)).toBeTrue();
    expect(s.endsWith("oobar", 6)).toBeTrue();
    expect(s.endsWith("foobar", 6)).toBeTrue();
    expect(s.endsWith("1foobar", 6)).toBeFalse();
    expect(s.endsWith("bar", [])).toBeFalse();
    expect(s.endsWith("bar", null)).toBeFalse();
    expect(s.endsWith("bar", false)).toBeFalse();
    expect(s.endsWith("bar", true)).toBeFalse();
    expect(s.endsWith("f", true)).toBeTrue();
    expect(s.endsWith("bar", -1)).toBeFalse();
    expect(s.endsWith("bar", 42)).toBeTrue();
    expect(s.endsWith("foo", 3)).toBeTrue();
    expect(s.endsWith("foo", "3")).toBeTrue();
    expect(s.endsWith("foo1", 3)).toBeFalse();
    expect(s.endsWith("foo", 3.7)).toBeTrue();
    expect(s.endsWith()).toBeFalse();
    expect(s.endsWith("")).toBeTrue();
    expect(s.endsWith("", 0)).toBeTrue();
    expect(s.endsWith("", 1)).toBeTrue();
    expect(s.endsWith("", -1)).toBeTrue();
    expect(s.endsWith("", 42)).toBeTrue();
    expect("12undefined".endsWith()).toBeTrue();
    expect(() => s.endsWith(/foobar/)).toThrowWithMessage(
        TypeError,
        "searchString is not a string, but a regular expression"
    );
    expect(s.endsWith("bar", undefined)).toBeTrue();
});

test("UTF-16", () => {
    var s = "😀";
    expect(s.endsWith("😀")).toBeTrue();
    expect(s.endsWith("\ud83d")).toBeFalse();
    expect(s.endsWith("\ude00")).toBeTrue();
    expect(s.endsWith("a")).toBeFalse();
});
