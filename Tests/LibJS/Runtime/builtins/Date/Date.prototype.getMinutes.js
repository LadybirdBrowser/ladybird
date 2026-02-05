test("basic functionality", () => {
    var d = new Date();
    expect(d.getMinutes()).toBe(d.getMinutes());
    expect(d.getMinutes()).not.toBeNaN();
    expect(d.getMinutes()).toBeGreaterThanOrEqual(0);
    expect(d.getMinutes()).toBeLessThanOrEqual(59);
    expect(new Date(NaN).getMinutes()).toBe(NaN);
});
