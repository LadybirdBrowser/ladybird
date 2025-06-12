test("basic functionality", () => {
    expect(Math.abs).toHaveLength(1);

    expect(Math.abs("-1")).toBe(1);
    expect(Math.abs(-2)).toBe(2);
    expect(Math.abs(null)).toBe(0);
    expect(Math.abs("")).toBe(0);
    expect(Math.abs([])).toBe(0);
    expect(Math.abs([2])).toBe(2);
    expect(Math.abs([1, 2])).toBeNaN();
    expect(Math.abs({})).toBeNaN();
    expect(Math.abs("string")).toBeNaN();
    expect(Math.abs()).toBeNaN();
});
