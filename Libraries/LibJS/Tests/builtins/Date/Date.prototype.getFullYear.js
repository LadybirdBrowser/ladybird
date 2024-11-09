test("basic functionality", () => {
    var d = new Date();
    expect(d.getFullYear()).toBe(d.getFullYear());
    expect(d.getFullYear()).not.toBeNaN();
    expect(d.getFullYear()).toBe(d.getFullYear());
    expect(d.getFullYear()).toBeGreaterThanOrEqual(2020);
    expect(new Date(NaN).getFullYear()).toBe(NaN);
});
