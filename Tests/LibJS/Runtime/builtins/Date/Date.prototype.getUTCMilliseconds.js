test("basic functionality", () => {
    var d = new Date();
    expect(d.getUTCMilliseconds()).toBe(d.getUTCMilliseconds());
    expect(d.getUTCMilliseconds()).not.toBeNaN();
    expect(d.getUTCMilliseconds()).toBeGreaterThanOrEqual(0);
    expect(d.getUTCMilliseconds()).toBeLessThanOrEqual(999);
    expect(new Date(NaN).getUTCMilliseconds()).toBe(NaN);
});
