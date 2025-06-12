test("basic functionality", () => {
    expect(String.fromCodePoint).toHaveLength(1);

    expect(String.fromCodePoint()).toBe("");
    expect(String.fromCodePoint(0)).toBe("\u0000");
    expect(String.fromCodePoint(false)).toBe("\u0000");
    expect(String.fromCodePoint(null)).toBe("\u0000");
    expect(String.fromCodePoint(1)).toBe("\u0001");
    expect(String.fromCodePoint(true)).toBe("\u0001");
    expect(String.fromCodePoint(0xffff)).toBe("\uffff");
    expect(String.fromCodePoint(65)).toBe("A");
    expect(String.fromCodePoint(65, 66, 67)).toBe("ABC");
    expect(String.fromCodePoint(228, 246, 252)).toBe("äöü");
});

test("errors", () => {
    expect(() => {
        String.fromCodePoint(NaN);
    }).toThrowWithMessage(
        RangeError,
        "must be an integer no less than 0 and no greater than 0x10FFFF"
    );

    expect(() => {
        String.fromCodePoint(-5);
    }).toThrowWithMessage(
        RangeError,
        "must be an integer no less than 0 and no greater than 0x10FFFF"
    );

    expect(() => {
        String.fromCodePoint(0x123ffff);
    }).toThrowWithMessage(
        RangeError,
        "must be an integer no less than 0 and no greater than 0x10FFFF"
    );
});
