test("basic functionality", () => {
    expect(Math.cos).toHaveLength(1);

    expect(Math.cos(0)).toBe(1);
    expect(Math.cos(null)).toBe(1);
    expect(Math.cos("")).toBe(1);
    expect(Math.cos([])).toBe(1);
    expect(Math.cos(Math.PI)).toBe(-1);
    expect(Math.cos()).toBeNaN();
    expect(Math.cos(undefined)).toBeNaN();
    expect(Math.cos([1, 2, 3])).toBeNaN();
    expect(Math.cos({})).toBeNaN();
    expect(Math.cos("foo")).toBeNaN();
    expect(Math.cos(-Infinity)).toBeNaN();
    expect(Math.cos(Infinity)).toBeNaN();
});
