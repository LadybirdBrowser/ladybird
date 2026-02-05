test("basic functionality", () => {
    var d = new Date();
    expect(d.getSeconds()).toBe(d.getSeconds());
    expect(d.getSeconds()).not.toBeNaN();
    expect(d.getSeconds()).toBeGreaterThanOrEqual(0);
    expect(d.getSeconds()).toBeLessThanOrEqual(59);
    expect(new Date(NaN).getSeconds()).toBe(NaN);
});
