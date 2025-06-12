test("basic functionality", () => {
    expect(String.fromCharCode).toHaveLength(1);

    expect(String.fromCharCode()).toBe("");
    expect(String.fromCharCode(0)).toBe("\u0000");
    expect(String.fromCharCode(false)).toBe("\u0000");
    expect(String.fromCharCode(null)).toBe("\u0000");
    expect(String.fromCharCode(undefined)).toBe("\u0000");
    expect(String.fromCharCode(1)).toBe("\u0001");
    expect(String.fromCharCode(true)).toBe("\u0001");
    expect(String.fromCharCode(-1)).toBe("\uffff");
    expect(String.fromCharCode(0xffff)).toBe("\uffff");
    expect(String.fromCharCode(0x123ffff)).toBe("\uffff");
    expect(String.fromCharCode(65)).toBe("A");
    expect(String.fromCharCode(65, 66, 67)).toBe("ABC");
    expect(String.fromCharCode(228, 246, 252)).toBe("äöü");
});
