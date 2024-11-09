test("basic functionality", () => {
    expect(Math.pow).toHaveLength(2);

    expect(Math.pow(2, 0)).toBe(1);
    expect(Math.pow(2, 1)).toBe(2);
    expect(Math.pow(2, 2)).toBe(4);
    expect(Math.pow(2, 3)).toBe(8);
    expect(Math.pow(2, -3)).toBe(0.125);
    expect(Math.pow(3, 2)).toBe(9);
    expect(Math.pow(0, 0)).toBe(1);
    expect(Math.pow(2, Math.pow(3, 2))).toBe(512);
    expect(Math.pow(Math.pow(2, 3), 2)).toBe(64);
    expect(Math.pow("2", "3")).toBe(8);
    expect(Math.pow("", [])).toBe(1);
    expect(Math.pow([], null)).toBe(1);
    expect(Math.pow(null, null)).toBe(1);
    expect(Math.pow(undefined, null)).toBe(1);
    expect(Math.pow(NaN, 2)).toBeNaN();
    expect(Math.pow(2, NaN)).toBeNaN();
    expect(Math.pow(undefined, 2)).toBeNaN();
    expect(Math.pow(2, undefined)).toBeNaN();
    expect(Math.pow(null, undefined)).toBeNaN();
    expect(Math.pow(2, "foo")).toBeNaN();
    expect(Math.pow("foo", 2)).toBeNaN();
});
