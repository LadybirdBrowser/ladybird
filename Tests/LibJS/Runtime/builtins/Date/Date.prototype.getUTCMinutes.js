test("basic functionality", () => {
    var d = new Date();
    expect(d.getUTCMinutes()).toBe(d.getUTCMinutes());
    expect(d.getUTCMinutes()).not.toBeNaN();
    expect(d.getUTCMinutes()).toBeGreaterThanOrEqual(0);
    expect(d.getUTCMinutes()).toBeLessThanOrEqual(59);
    expect(new Date(NaN).getUTCMinutes()).toBe(NaN);
});
