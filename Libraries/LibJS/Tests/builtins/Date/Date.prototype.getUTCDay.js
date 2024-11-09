test("basic functionality", () => {
    var d = new Date();
    expect(d.getUTCDay()).toBe(d.getUTCDay());
    expect(d.getUTCDay()).not.toBeNaN();
    expect(d.getUTCDay()).toBeGreaterThanOrEqual(0);
    expect(d.getUTCDay()).toBeLessThanOrEqual(6);
    expect(new Date(NaN).getUTCDay()).toBe(NaN);
});
