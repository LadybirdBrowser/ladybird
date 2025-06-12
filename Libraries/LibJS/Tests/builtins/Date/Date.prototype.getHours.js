test("basic functionality", () => {
    var d = new Date();
    expect(d.getHours()).toBe(d.getHours());
    expect(d.getHours()).not.toBeNaN();
    expect(d.getHours()).toBeGreaterThanOrEqual(0);
    expect(d.getHours()).toBeLessThanOrEqual(23);
    expect(new Date(NaN).getHours()).toBe(NaN);
});
