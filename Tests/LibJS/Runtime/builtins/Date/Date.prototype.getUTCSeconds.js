test("basic functionality", () => {
    var d = new Date();
    expect(d.getUTCSeconds()).toBe(d.getUTCSeconds());
    expect(d.getUTCSeconds()).not.toBeNaN();
    expect(d.getUTCSeconds()).toBeGreaterThanOrEqual(0);
    expect(d.getUTCSeconds()).toBeLessThanOrEqual(59);
    expect(new Date(NaN).getUTCSeconds()).toBe(NaN);
});
