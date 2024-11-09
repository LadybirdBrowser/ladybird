test("basic functionality", () => {
    var d = new Date();
    expect(d.getUTCHours()).toBe(d.getUTCHours());
    expect(d.getUTCHours()).not.toBeNaN();
    expect(d.getUTCHours()).toBeGreaterThanOrEqual(0);
    expect(d.getUTCHours()).toBeLessThanOrEqual(23);
    expect(new Date(NaN).getUTCHours()).toBe(NaN);
});
