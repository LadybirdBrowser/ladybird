test("basic functionality", () => {
    expect(Math.sin).toHaveLength(1);

    expect(Math.sin(0)).toBe(0);
    expect(Math.sin(null)).toBe(0);
    expect(Math.sin("")).toBe(0);
    expect(Math.sin([])).toBe(0);
    expect(Math.sin((Math.PI * 3) / 2)).toBe(-1);
    expect(Math.sin(Math.PI / 2)).toBe(1);
    expect(Math.sin()).toBeNaN();
    expect(Math.sin(undefined)).toBeNaN();
    expect(Math.sin([1, 2, 3])).toBeNaN();
    expect(Math.sin({})).toBeNaN();
    expect(Math.sin("foo")).toBeNaN();
    expect(Math.sin(Infinity)).toBeNaN();
    expect(Math.sin(-Infinity)).toBeNaN();
});
