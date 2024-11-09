test("basic functionality", () => {
    var d = new Date();
    expect(d.getMilliseconds()).toBe(d.getMilliseconds());
    expect(d.getMilliseconds()).not.toBeNaN();
    expect(d.getMilliseconds()).toBeGreaterThanOrEqual(0);
    expect(d.getMilliseconds()).toBeLessThanOrEqual(999);
    expect(new Date(NaN).getMilliseconds()).toBe(NaN);
});
