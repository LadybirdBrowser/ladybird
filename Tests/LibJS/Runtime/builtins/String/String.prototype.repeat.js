test("basic functionality", () => {
    expect(String.prototype.repeat).toHaveLength(1);

    expect("foo".repeat(0)).toBe("");
    expect("foo".repeat(1)).toBe("foo");
    expect("foo".repeat(2)).toBe("foofoo");
    expect("foo".repeat(3)).toBe("foofoofoo");
    expect("foo".repeat(3.1)).toBe("foofoofoo");
    expect("foo".repeat(3.5)).toBe("foofoofoo");
    expect("foo".repeat(3.9)).toBe("foofoofoo");
    expect("foo".repeat(null)).toBe("");
    expect("foo".repeat(undefined)).toBe("");
    expect("foo".repeat([])).toBe("");
    expect("foo".repeat("")).toBe("");
});

test("throws correct range errors", () => {
    expect(() => {
        "foo".repeat(-1);
    }).toThrowWithMessage(RangeError, "repeat count must be a positive number");

    expect(() => {
        "foo".repeat(Infinity);
    }).toThrowWithMessage(RangeError, "repeat count must be a finite number");

    expect(() => {
        "foo".repeat(0xffffffffff);
    }).toThrowWithMessage(RangeError, "repeat count must not overflow");
});

test("UTF-16", () => {
    expect("😀".repeat(0)).toBe("");
    expect("😀".repeat(1)).toBe("😀");
    expect("😀".repeat(10)).toBe("😀😀😀😀😀😀😀😀😀😀");
});
