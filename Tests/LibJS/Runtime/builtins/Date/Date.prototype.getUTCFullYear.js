test("basic functionality", () => {
    var d = new Date();
    expect(d.getUTCFullYear()).toBe(d.getUTCFullYear());
    expect(d.getUTCFullYear()).not.toBeNaN();
    expect(d.getUTCFullYear()).toBe(d.getUTCFullYear());
    expect(d.getUTCFullYear()).toBeGreaterThanOrEqual(2020);
    expect(new Date(NaN).getUTCFullYear()).toBe(NaN);
});
