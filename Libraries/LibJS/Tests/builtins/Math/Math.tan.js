test("basic functionality", () => {
    expect(Math.tan).toHaveLength(1);

    expect(Math.tan(0)).toBe(0);
    expect(Math.tan(null)).toBe(0);
    expect(Math.tan("")).toBe(0);
    expect(Math.tan([])).toBe(0);
    expect(Math.ceil(Math.tan(Math.PI / 4))).toBe(1);
    expect(Math.tan()).toBeNaN();
    expect(Math.tan(undefined)).toBeNaN();
    expect(Math.tan([1, 2, 3])).toBeNaN();
    expect(Math.tan({})).toBeNaN();
    expect(Math.tan("foo")).toBeNaN();
    expect(Math.tan(Infinity)).toBeNaN();
    expect(Math.tan(-Infinity)).toBeNaN();
});
