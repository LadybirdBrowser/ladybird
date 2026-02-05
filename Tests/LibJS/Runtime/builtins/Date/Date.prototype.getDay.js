test("basic functionality", () => {
    var d = new Date();
    expect(d.getDay()).toBe(d.getDay());
    expect(d.getDay()).not.toBeNaN();
    expect(d.getDay()).toBeGreaterThanOrEqual(0);
    expect(d.getDay()).toBeLessThanOrEqual(6);
    expect(new Date(NaN).getDay()).toBe(NaN);
});
